from random import random
from time import time

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.middleware.cors import CORSMiddleware
import asyncio
import uvicorn
import struct
import json
import networkx as nx
from pydantic import BaseModel
import os 
import re
import math
from serial_manager import SerialManager
import numpy as np
import traceback
app = FastAPI(title="ElectroPhy Command Center")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"], 
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

hardware = SerialManager(port=None, baudrate=115200)

@app.get("/")
def read_root():
    return {"status": "ElectroPhy Backend is Running"}

@app.get("/api/hardware/connect")
def connect_hardware():
    # Force FastAPI to always say "connected" so React doesn't panic
    if hardware.is_running:
        return {"status": "connected", "port": hardware.port or "Wi-Fi 8080"}
        
    success = hardware.connect()
    if success:
        return {"status": "connected", "port": hardware.port or "Wi-Fi 8080"}
        
    return {"status": "failed to connect", "error": "Device not found"}
@app.get("/api/hardware/disconnect")
def disconnect_hardware():
    hardware.disconnect()
    return {"status": "disconnected"}

@app.post("/api/experiment/start")
def start_experiment():
    hardware.start_recording()
    return {"status": "recording started"}

@app.post("/api/experiment/stop")
def stop_experiment():
    hardware.stop_recording()
    return {"status": "recording stopped", "buffered_points": len(hardware.experiment_buffer)}

@app.post("/api/experiment/clear")
def clear_experiment():
    hardware.clear_buffer()
    return {"status": "buffer cleared"}

@app.post("/api/experiment/save")
def save_experiment(filename: str = "latest_run.csv"):
    success = hardware.save_buffer_to_disk(filename)
    if success:
        return {"status": "saved", "file": filename}
    return {"status": "error", "message": "Failed to save"}

@app.post("/api/stream/start")
def start_stream():
    hardware.is_streaming = True
    hardware.latest_data = None # Clear stale data
    return {"status": "streaming started"}

@app.post("/api/stream/stop")
def stop_stream():
    hardware.is_streaming = False
    return {"status": "streaming stopped"}

# --- Block Registry Loading ---
REGISTRY_PATH = os.path.join(os.path.dirname(__file__), "block_registry.json")
try:
    with open(REGISTRY_PATH, "r") as f:
        BLOCK_REGISTRY = json.load(f)
except FileNotFoundError:
    print(f"WARNING: {REGISTRY_PATH} not found!")
    BLOCK_REGISTRY = {}

OPCODE_MAP = {data["id"]: int(opcode, 16) for opcode, data in BLOCK_REGISTRY.items()}
DOMAIN_MAP = {data["id"]: data.get("domain", "chip") for opcode, data in BLOCK_REGISTRY.items()}

# --- TRUE DATA ROUTER STATE ---
pc_pipeline_state = {"nodes": [], "wires": []}

class GraphPayload(BaseModel):
    nodes: list
    edges: list

@app.get("/api/blocks/registry")
def get_block_registry():
    return BLOCK_REGISTRY

# ==========================================
# GLOBAL HELPERS
# ==========================================
def get_pin_index(handle_str):
    if not handle_str: 
        return 0
        
    s = str(handle_str).lower()
    
    if 'trace' in s:
        match = re.search(r'\d+', s)
        if match: 
            return max(0, int(match.group()) - 1)
            
    if s == 'x': return 0
    if s == 'y': return 1
    if s == 'z': return 2
            
    match = re.search(r'\d+', s)
    if match:
        return int(match.group())
        
    return 0

# ==========================================
# COMPILER
# ==========================================
def compile_graph_to_bytecode(nodes, edges):
    global pc_pipeline_state
    pc_pipeline_state = {"nodes": [], "wires": []}
    
    G = nx.MultiDiGraph()
    for node in nodes:
        node_type = node.get("data", {}).get("id")
        if node_type in OPCODE_MAP:
            domain = DOMAIN_MAP.get(node_type, "chip")
            G.add_node(node["id"], type=node_type, domain=domain, params=node.get("data", {}))

    for edge in edges:
        G.add_edge(edge["source"], edge["target"], 
                   sourceHandle=edge.get("sourceHandle", "out_0"),
                   targetHandle=edge.get("targetHandle", "in_0"))

    if not nx.is_directed_acyclic_graph(G):
        raise ValueError("Graph contains a cycle.")

    execution_order = list(nx.topological_sort(G))
    pico_nodes = [n for n in execution_order if G.nodes[n].get("domain") == "chip"]
    pc_nodes = [n for n in execution_order if G.nodes[n].get("domain") == "pc"]

    pc_subgraph = G.subgraph(pc_nodes)
    try:
        pc_exec_order = list(nx.topological_sort(pc_subgraph))
    except nx.NetworkXUnfeasible:
        pc_exec_order = pc_nodes 

    pc_pipeline_state["nodes"] = [
        {"id": n, "op": G.nodes[n]["type"], "params": G.nodes[n].get("params", {})}
        for n in pc_exec_order
    ]

    pc_pipeline_state["wires"] = [
        {
            "source": u,
            "sourceHandle": data.get("sourceHandle", "out_0"),
            "target": v,
            "targetHandle": data.get("targetHandle", "in_0")
        }
        for u, v, data in G.edges(data=True) if u in pc_nodes and v in pc_nodes
    ]

    register_map = {}
    reg_counter = 0
    
    node_inputs = {n: [255] * 6 for n in pico_nodes}
    node_outputs = {n: [255] * 6 for n in pico_nodes}

    for source, target, edge_data in G.edges(data=True):
        if source in pico_nodes and target in pico_nodes:
            src_handle = edge_data.get("sourceHandle", "0")
            tgt_handle = edge_data.get("targetHandle", "0")
            
            src_idx = get_pin_index(src_handle)
            tgt_idx = get_pin_index(tgt_handle)

            edge_id = f"{source}_{src_handle}_to_{target}_{tgt_handle}"
            if edge_id not in register_map:
                register_map[edge_id] = reg_counter
                reg_counter += 1

            node_outputs[source][src_idx] = register_map[edge_id]
            node_inputs[target][tgt_idx] = register_map[edge_id]

    binary_payload = bytearray()
    instruction_count = 0

    for node_id in pico_nodes:
        n_data = G.nodes[node_id]
        opcode = OPCODE_MAP[n_data["type"]]
        
        in_regs = node_inputs[node_id]
        out_regs = node_outputs[node_id]

        raw_val = n_data.get("params", {}).get("threshold", 0)
        try:
            param_val = float(raw_val) if str(raw_val).strip() != "" else 0.0
        except ValueError:
            param_val = 0.0

        inst_bytes = struct.pack('<BBBBBBBBBBBBBf', 
            opcode, 
            in_regs[0], in_regs[1], in_regs[2], in_regs[3], in_regs[4], in_regs[5],
            out_regs[0], out_regs[1], out_regs[2], out_regs[3], out_regs[4], out_regs[5],
            param_val
        )
        binary_payload.extend(inst_bytes)
        instruction_count += 1
        
    if instruction_count == 0:
        dummy = struct.pack('<BBBBBBBBBBBBBf', 0x00, 255,255,255,255,255,255, 255,255,255,255,255,255, 0.0)
        binary_payload.extend(dummy)
        instruction_count = 1

    header = struct.pack('<BB', 0xAA, instruction_count)
    return header + binary_payload

@app.post("/api/experiment/compile")
def compile_experiment(payload: GraphPayload):
    if not hardware.is_running: raise HTTPException(status_code=400, detail="Hardware not connected")
    
    # 1. Turn OFF stream during compile
    hardware.is_streaming = False 
    node_state_memory.clear()     
    hardware.latest_data = None   

    try:
        binary_data = compile_graph_to_bytecode(payload.nodes, payload.edges)
        
        import time
        for byte in binary_data:
            hardware.send_command(bytes([byte]))
            time.sleep(0.01) 
        
        # 2. TURN IT BACK ON!!! (This is what was missing)
        hardware.is_streaming = True
        
        print(f"\n[COMPILER] Graph Deployed. Trickle-fed {len(binary_data)} bytes via Wi-Fi.")
        return {"status": "compiled", "bytes_sent": len(binary_data)}
        
    except Exception as e:
        import traceback
        traceback.print_exc()
        raise HTTPException(status_code=500, detail="Internal compiler failure")
    
# ==========================================
# REAL-TIME DATA ENGINE
# ==========================================
node_state_memory = {} 

@app.websocket("/ws/stream")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    global node_state_memory
    node_state_memory.clear() 
    print("\n[WEBSOCKET] React UI Connected. Listening for live data...")
    
    try:
        while True:
            plots_to_send = None
            frames_processed = 0
            
            # Drain queue safely (max 20 frames per tick to prevent blocking)
            while hardware.is_running and not hardware.data_queue.empty():
                try:
                    raw_string = hardware.data_queue.get_nowait()
                    
                    # Keep terminal alive so you know data is arriving
                    print(f"\r[DATA TICK] {raw_string[:40]:<40}", end="", flush=True)
                    
                    if "[DEBUG]" not in raw_string:
                        clean_data = raw_string.replace("--> [HARDWARE RX]", "").strip()
                        
                        # --- THE REAL FIX: BULLETPROOF NUMBER EXTRACTION ---
                        # This ignores all Wi-Fi null bytes (\x00), carriage returns, and garbage.
                        import re
                        numbers = re.findall(r'-?\d+\.\d+|-?\d+', clean_data)
                        hw_vals = [float(x) for x in numbers]

                        if pc_pipeline_state["nodes"] and hw_vals:
                            node_state = {}
                            frame_plots = {}
                            
                            # 1. FUZZY MATCH: Find any node acting as "RX"
                            for n in pc_pipeline_state["nodes"]:
                                op_str = str(n["op"]).lower()
                                if "transport" in op_str or "rx" in op_str or "in" in op_str:
                                    node_state[n["id"]] = hw_vals
                            
                            time_plot_count = 1
                            for n in pc_pipeline_state["nodes"]:
                                nid, op_str = n["id"], str(n["op"]).lower()
                                if "transport" in op_str or "rx" in op_str: continue
                                if nid not in node_state_memory: node_state_memory[nid] = {}

                                inputs = {} 
                                for w in pc_pipeline_state["wires"]:
                                    if w["target"] == nid:
                                        src_idx, tgt_idx = get_pin_index(w["sourceHandle"]), get_pin_index(w["targetHandle"])
                                        src_data = node_state.get(w["source"], [])
                                        inputs[tgt_idx] = src_data[src_idx] if src_idx < len(src_data) else 0.0

                                node_state[nid] = [0.0] * 6 

                                # --- DSP MATH OPERATIONS ---
                                if "mag" in op_str: 
                                    node_state[nid][0] = math.sqrt(inputs.get(0, 0)**2 + inputs.get(1, 0)**2 + inputs.get(2, 0)**2)
                                elif "add" in op_str: 
                                    node_state[nid][0] = inputs.get(0, 0.0) + float(n["params"].get("threshold", 0.0))
                                # --- NEW MULTIPLICATION BLOCK ---
                                elif "mul" in op_str:
                                    try: 
                                        multiplier = float(n["params"].get("threshold", 1.0))
                                    except ValueError: 
                                        multiplier = 1.0
                                    node_state[nid][0] = inputs.get(0, 0.0) * multiplier
                                # --------------------------------
                                elif "hpf" in op_str:
                                    in_val = inputs.get(0, 0.0)
                                    try: fc = float(n["params"].get("threshold", 2.0))
                                    except ValueError: fc = 2.0
                                    dt = 0.01 
                                    rc = 1.0 / (2 * math.pi * fc) if fc > 0 else 1.0
                                    alpha = rc / (rc + dt)
                                    x_prev = node_state_memory[nid].get("x_prev", in_val)
                                    y_prev = node_state_memory[nid].get("y_prev", 0.0)
                                    y = alpha * (y_prev + in_val - x_prev)
                                    node_state_memory[nid]["x_prev"] = in_val
                                    node_state_memory[nid]["y_prev"] = y
                                    node_state[nid][0] = y
                                elif "lpf" in op_str:
                                    in_val = inputs.get(0, 0.0)
                                    try: fc = float(n["params"].get("threshold", 5.0)) 
                                    except ValueError: fc = 5.0
                                    dt = 0.01 
                                    rc = 1.0 / (2 * math.pi * fc) if fc > 0 else 1.0
                                    alpha = dt / (rc + dt)
                                    y_prev = node_state_memory[nid].get("y_prev", in_val)
                                    y = y_prev + alpha * (in_val - y_prev)
                                    node_state_memory[nid]["y_prev"] = y
                                    node_state[nid][0] = y
                                elif "fft" in op_str:
                                    in_val = inputs.get(0, 0.0)
                                    if "buffer" not in node_state_memory[nid]:
                                        node_state_memory[nid]["buffer"] = [] 
                                    node_state_memory[nid]["buffer"].append(in_val)
                                    if len(node_state_memory[nid]["buffer"]) >= 64:
                                        fft_data = np.abs(np.fft.rfft(node_state_memory[nid]["buffer"]))
                                        fft_data[0] = 0.0 
                                        node_state[nid][0] = fft_data.tolist()
                                        node_state_memory[nid]["buffer"].clear()
                                    else:
                                        node_state[nid][0] = []
                                        
                                # --- SYNTHETIC DATA GENERATORS ---
                                elif "source_sine" in op_str:
                                    # Use the UI input box to set noise level (defaults to 0.1)
                                    try: noise_amp = float(n.get("params", {}).get("threshold", 0.1))
                                    except ValueError: noise_amp = 0.1
                                    
                                    t = time.time()
                                    # Generates a 1Hz sine wave + random noise
                                    val = math.sin(t * 2 * math.pi) + random.uniform(-noise_amp, noise_amp)
                                    node_state[nid][0] = val

                                elif "source_rc" in op_str:
                                    try: noise_amp = float(n.get("params", {}).get("threshold", 0.05))
                                    except ValueError: noise_amp = 0.05
                                    
                                    # Modulo 5 creates a looping 5-second RC charge curve
                                    t = time.time() % 5.0 
                                    # Standard RC capacitor charge equation (aiming for 3.3V)
                                    val = 3.3 * (1.0 - math.exp(-t / 1.0)) + random.uniform(-noise_amp, noise_amp)
                                    node_state[nid][0] = val

                                # --- SCATTER PLOT ROUTING ---
                                elif "plot_scatter" in op_str:
                                    x_val = inputs.get(0, 0.0)
                                    y_val = inputs.get(1, 0.0)
                                    # We send it as a special "scatter" type so React knows how to render it
                                    frame_plots[nid] = {"type": "scatter", "title": "X/Y SCATTER", "data": [x_val, y_val]}
                                    
                                        
                                # --- PLOT ROUTING ---
                                elif "plot" in op_str and "freq" not in op_str:
                                    traces = [inputs[i] for i in range(6) if i in inputs]
                                    if traces:
                                        frame_plots[nid] = {"type": "time", "title": f"TIME SERIES {time_plot_count}", "data": traces}
                                        time_plot_count += 1
                                        
                                elif "freq" in op_str:
                                    if isinstance(inputs.get(0, []), list) and len(inputs.get(0, [])) > 0:
                                        frame_plots[nid] = {"type": "freq", "title": "FREQUENCY SPECTRUM", "data": inputs.get(0, [])}
                                        
                            if frame_plots:
                                plots_to_send = frame_plots # Overwrite with the latest frame
                                
                except Exception as e:
                    # If math completely fails, print it so we actually see the bug!
                    if "ClientDisconnected" not in str(e) and "close message" not in str(e):
                        print(f"\n[PARSE ERROR] Dropped frame: {e} | Raw Data: {repr(raw_string)}")

                frames_processed += 1
                if frames_processed > 20: 
                    break # Safety exit to prevent async lockup
            
            # Send ONLY the latest batched frame to React (fixes the UI freeze)
            if plots_to_send:
                await websocket.send_text(json.dumps(plots_to_send))

            # Enforce 50 FPS max to keep React UI buttery smooth and prevent lockups
            await asyncio.sleep(0.02) 
            
    except WebSocketDisconnect:
        print("\n[WEBSOCKET] React UI Disconnected!")

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8000)