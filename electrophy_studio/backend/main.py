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
from collections import deque
import json
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
    if hardware.is_running:
        return {"status": "already connected", "port": hardware.port}
    success = hardware.connect()
    if success:
        return {"status": "connected", "port": hardware.port}
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

    # =========================================================
    # UPGRADED: Save PC Nodes AND PC Wires for Execution Engine
    # =========================================================
    pc_subgraph = G.subgraph(pc_nodes)
    try:
        pc_exec_order = list(nx.topological_sort(pc_subgraph))
    except nx.NetworkXUnfeasible:
        pc_exec_order = pc_nodes # Fallback

    pc_pipeline_state["nodes"] = [
        {"id": n, "op": G.nodes[n]["type"], "params": G.nodes[n].get("params", {})}
        for n in pc_exec_order
    ]

    # Map the wires connecting the PC blocks together
    pc_pipeline_state["wires"] = [
        {
            "source": u,
            "sourceHandle": data.get("sourceHandle", "out_0"),
            "target": v,
            "targetHandle": data.get("targetHandle", "in_0")
        }
        for u, v, data in G.edges(data=True) if u in pc_nodes and v in pc_nodes
    ]
    # =========================================================

    # --- 2. BUILD PICO BYTECODE ---
    register_map = {}
    reg_counter = 0
    
    node_inputs = {n: [255] * 6 for n in pico_nodes}
    node_outputs = {n: [255] * 6 for n in pico_nodes}

    def get_pin_index(handle_str):
        s = str(handle_str).lower()
        match = re.search(r'\d+', s)
        if match:
            idx = int(match.group())
            return idx if idx <= 5 else 0
            
        if 'x' in s and 'accel' in s: return 0
        if 'y' in s and 'accel' in s: return 1
        if 'z' in s and 'accel' in s: return 2
        if 'pitch' in s or ('x' in s and 'gyro' in s): return 3
        if 'roll' in s or ('y' in s and 'gyro' in s): return 4
        if 'yaw' in s or ('z' in s and 'gyro' in s): return 5
        return 0

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

    header = struct.pack('<BB', 0xAA, instruction_count)
    return header + binary_payload

@app.post("/api/experiment/compile")
def compile_experiment(payload: GraphPayload):
    if not hardware.is_running:
        raise HTTPException(status_code=400, detail="Hardware not connected")

    try:
        binary_data = compile_graph_to_bytecode(payload.nodes, payload.edges)
        hardware.send_command(binary_data)
        print(f"Graph Compiled. Sent {len(binary_data)} bytes to Pico.")
        return {"status": "compiled", "bytes_sent": len(binary_data)}
        
    except ValueError as ve:
        raise HTTPException(status_code=400, detail=str(ve))
    except Exception as e:
        raise HTTPException(status_code=500, detail="Internal compiler failure")


# --- REAL-TIME DATA ENGINE ---
node_state_memory = {} # Keeps memory for filters and FFT buffers between ticks

@app.websocket("/ws/stream")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()
    global node_state_memory
    node_state_memory.clear() # Reset memory on new connection
    
    last_sent_data = None
    
    try:
        while True:
            if hardware.is_running and hardware.latest_data:
                raw_string = hardware.latest_data
                
                if raw_string != last_sent_data:
                    last_sent_data = raw_string
                    
                    if "[DEBUG]" not in raw_string:
                        clean_data = raw_string.replace("--> [HARDWARE RX]", "").strip()
                        
                        if clean_data:
                            try:
                                hw_vals = [float(x) for x in clean_data.split(',')]
                            except ValueError:
                                hw_vals = []

                            if pc_pipeline_state["nodes"] and hw_vals:
                                node_state = {}
                                plots_to_send = {}
                                
                                for n in pc_pipeline_state["nodes"]:
                                    if n["op"] == "transport_in":
                                        node_state[n["id"]] = {f"out_{i}": val for i, val in enumerate(hw_vals)}
                                
                                time_plot_count = 1

                                for n in pc_pipeline_state["nodes"]:
                                    nid = n["id"]
                                    op = n["op"]
                                    if op == "transport_in": continue

                                    if nid not in node_state_memory:
                                        node_state_memory[nid] = {}

                                    inputs = {}
                                    for w in pc_pipeline_state["wires"]:
                                        if w["target"] == nid:
                                            inputs[w["targetHandle"]] = node_state.get(w["source"], {}).get(w["sourceHandle"], 0.0)

                                    node_state[nid] = {}

                                    # ------------------------------------------------
                                    # DSP MATH OPERATIONS
                                    # ------------------------------------------------
                                    if op == "math_mag":
                                        x, y, z = inputs.get("in_0", 0.0), inputs.get("in_1", 0.0), inputs.get("in_2", 0.0)
                                        node_state[nid]["out_0"] = math.sqrt(x*x + y*y + z*z)

                                    elif op == "math_add":
                                        try: const = float(n["params"].get("threshold", 0.0))
                                        except ValueError: const = 0.0
                                        node_state[nid]["out_0"] = inputs.get("in_0", 0.0) + const

                                    elif op == "math_hpf":
                                        in_val = inputs.get("in_0", 0.0)
                                        try: fc = float(n["params"].get("threshold", 2.0)) # Default 2Hz cutoff
                                        except ValueError: fc = 2.0
                                        
                                        dt = 0.01 # Assuming 100Hz loop rate from Pico
                                        rc = 1.0 / (2 * math.pi * fc) if fc > 0 else 1.0
                                        alpha = rc / (rc + dt)
                                        
                                        x_prev = node_state_memory[nid].get("x_prev", in_val)
                                        y_prev = node_state_memory[nid].get("y_prev", 0.0)
                                        
                                        y = alpha * (y_prev + in_val - x_prev)
                                        node_state_memory[nid]["x_prev"] = in_val
                                        node_state_memory[nid]["y_prev"] = y
                                        node_state[nid]["out_0"] = y

                                    elif op == "math_fft":
                                        in_val = inputs.get("in_0", 0.0)
                                        if "buffer" not in node_state_memory[nid]:
                                            # Remove maxlen so we can clear it manually
                                            node_state_memory[nid]["buffer"] = [] 
                                            
                                        node_state_memory[nid]["buffer"].append(in_val)
                                        
                                        # Only compute and send FFT every 64 samples
                                        if len(node_state_memory[nid]["buffer"]) >= 64:
                                            fft_data = np.abs(np.fft.rfft(node_state_memory[nid]["buffer"]))
                                            fft_data[0] = 0.0 # Drop DC offset
                                            node_state[nid]["out_0"] = fft_data.tolist()
                                            
                                            # FLUSH THE BUFFER!
                                            node_state_memory[nid]["buffer"].clear()
                                        else:
                                            node_state[nid]["out_0"] = []
                                    
                                    
                                            
                                    

                                    # ------------------------------------------------
                                    # PLOT ROUTING
                                    # ------------------------------------------------
                                    elif op == "plot":
                                        traces = [inputs[f"in_{i}"] for i in range(4) if f"in_{i}" in inputs]
                                        if traces:
                                            # Add the counter to the title!
                                            plots_to_send[nid] = {"type": "time", "title": f"TIME SERIES {time_plot_count}", "data": traces}
                                            time_plot_count += 1
                                            
                                    elif op == "plot_freq":
                                        if "in_0" in inputs and isinstance(inputs["in_0"], list) and len(inputs["in_0"]) > 0:
                                            plots_to_send[nid] = {"type": "freq", "title": "FREQUENCY SPECTRUM", "data": inputs["in_0"]}
                                            
                                # Send JSON packet of all active plots
                                if plots_to_send:
                                    await websocket.send_text(json.dumps(plots_to_send))

            await asyncio.sleep(0.01)
            
    except WebSocketDisconnect:
        print("\n[WEBSOCKET] React UI Disconnected!")
        
if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)