import React, { useState, useEffect, useRef, useCallback } from 'react';
import ReactFlow, {
  ReactFlowProvider, useNodesState, useEdgesState, addEdge,
  Background
} from 'reactflow';
import 'reactflow/dist/style.css';

// Import our modularized components
import Sidebar from './Sidebar';
import { nodeTypes, ThemeContext, DarkModeContext } from './CustomNodes';

let globalNodeId = 0;
// Helper to ensure we don't have overlapping IDs when loading new flows
const getId = () => `node_${Date.now()}_${globalNodeId++}`;

const ExperimentBuilder = ({ theme, isDarkMode, onGoToPlot }) => {
  const reactFlowWrapper = useRef(null);
  const fileInputRef = useRef(null); // Reference for the hidden file input
  const [nodes, setNodes, onNodesChange] = useNodesState([]);
  const [edges, setEdges, onEdgesChange] = useEdgesState([]);
  const [reactFlowInstance, setReactFlowInstance] = useState(null);
  const [registry, setRegistry] = useState(null);
  const [experimentName, setExperimentName] = useState("");

  useEffect(() => {
    fetch('http://localhost:8000/api/blocks/registry')
      .then(res => res.json())
      .then(data => {
        const grouped = {};
        const typeMapping = { sensor: 'sensorNode', logic: 'logicNode', math: 'mathNode', action: 'actionNode' };
        const colors = { chip: '#3b82f6', pc: '#9333ea' };

        Object.values(data).forEach(block => {
          if (!grouped[block.category]) grouped[block.category] = [];
          block.color = colors[block.domain] || '#ccc'; 
          block.type = typeMapping[block.category] || 'actionNode';
          grouped[block.category].push(block);
        });
        setRegistry(grouped);
      })
      .catch(err => console.error("Failed to load blocks", err));
  }, []);

  const onConnect = useCallback((params) => setEdges((eds) => addEdge({ ...params, style: { stroke: theme?.accent, strokeWidth: 2 } }, eds)), [setEdges, theme]);

  const onDrop = useCallback((event) => {
    event.preventDefault();
    const bounds = reactFlowWrapper.current.getBoundingClientRect();
    const rawData = event.dataTransfer.getData('application/reactflow');
    if (!rawData) return;

    const nodeTemplate = JSON.parse(rawData);
    const position = reactFlowInstance.project({ x: event.clientX - bounds.left, y: event.clientY - bounds.top });

    setNodes((nds) => nds.concat({
      id: getId(), type: nodeTemplate.type, position,
      data: { ...nodeTemplate, operator: '>', threshold: '' }
    }));
  }, [reactFlowInstance, setNodes]);


  // 1. Save the Graph Layout (Nodes & Edges) as a JSON file locally
  const handleSaveFlow = () => {
    const flowData = { nodes, edges };
    const fileName = experimentName.trim() ? `${experimentName}_config.json` : "experiment_config.json";
    
    const dataStr = "data:text/json;charset=utf-8," + encodeURIComponent(JSON.stringify(flowData, null, 2));
    const downloadAnchorNode = document.createElement('a');
    downloadAnchorNode.setAttribute("href", dataStr);
    downloadAnchorNode.setAttribute("download", fileName);
    document.body.appendChild(downloadAnchorNode);
    downloadAnchorNode.click();
    downloadAnchorNode.remove();
  };

  // 2. Load a Graph Layout from a JSON file
  const handleLoadFlowClick = () => {
    if (fileInputRef.current) {
      fileInputRef.current.click();
    }
  };

  const handleFileChange = (event) => {
    const file = event.target.files[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = (e) => {
      try {
        const flowData = JSON.parse(e.target.result);
        
        if (flowData.nodes && flowData.edges) {
          setNodes(flowData.nodes || []);
          setEdges(flowData.edges || []);
          
          // Try to set the experiment name based on the filename
          const cleanName = file.name.replace('_config.json', '').replace('.json', '');
          setExperimentName(cleanName);
        } else {
          alert("Invalid configuration file. Missing nodes or edges.");
        }
      } catch (error) {
        console.error("Error parsing JSON:", error);
        alert("Failed to parse the configuration file.");
      }
    };
    reader.readAsText(file);
    
    // Reset the input value so the same file can be selected again if needed
    event.target.value = null; 
  };

  // 3. Trigger the Backend to save the hardware run data to CSV
  const handleSaveData = async () => {
    const fileName = experimentName.trim() ? `${experimentName}.csv` : "latest_run.csv";
    try {
      const res = await fetch(`http://localhost:8000/api/experiment/save?filename=${fileName}`, {
        method: 'POST'
      });
      const result = await res.json();
      if (res.ok) {
        alert(`Hardware data saved to ${result.file}`);
      } else {
        alert(`Error: ${result.message}`);
      }
    } catch (err) {
      alert('Backend unreachable.');
    }
  };

  const handleCompile = async () => {
    try {
      const res = await fetch('http://localhost:8000/api/experiment/compile', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ nodes, edges }),
      });
      const result = await res.json();
      if (res.ok) alert(`Compiled successfully. HW Bytes: ${result.bytes_sent}`);
      else alert(`Error: ${result.detail || 'Compiler failed'}`);
    } catch { alert('Backend unreachable.'); }
  };

  if (!theme) return null;

  return (
    <ThemeContext.Provider value={theme}>
      <DarkModeContext.Provider value={isDarkMode}>
        <div style={{ display: 'flex', flexDirection: 'column', height: '100%', width: '100%' }}>
          
          {/* UPDATED TOOLBAR */}
          <div style={{ padding: '8px 16px', backgroundColor: theme.panelHeader, borderBottom: `1px solid ${theme.border}`, display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
            
            {/* Left side: Flow & Data Management */}
            <div style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
              <input 
                type="text" 
                placeholder="Experiment Name..." 
                value={experimentName}
                onChange={(e) => setExperimentName(e.target.value)}
                style={{ padding: '6px 10px', backgroundColor: theme.bg, border: `1px solid ${theme.border}`, color: theme.textMain, fontFamily: theme.monoFont, fontSize: '12px', borderRadius: theme.radius, width: '200px', outline: 'none' }}
              />
              
              <button 
                onClick={handleSaveFlow}
                style={{ padding: '6px 12px', backgroundColor: theme.panelBg, color: theme.textMain, border: `1px solid ${theme.border}`, borderRadius: theme.radius, cursor: 'pointer', fontFamily: theme.monoFont, fontSize: '11px', fontWeight: 'bold' }}>
                SAVE FLOW
              </button>
              
              <button 
                onClick={handleLoadFlowClick}
                style={{ padding: '6px 12px', backgroundColor: theme.panelBg, color: theme.textMain, border: `1px solid ${theme.border}`, borderRadius: theme.radius, cursor: 'pointer', fontFamily: theme.monoFont, fontSize: '11px', fontWeight: 'bold' }}>
                LOAD FLOW
              </button>

              <button 
                onClick={handleSaveData}
                style={{ padding: '6px 12px', backgroundColor: theme.panelBg, color: theme.textMain, border: `1px solid ${theme.border}`, borderRadius: theme.radius, cursor: 'pointer', fontFamily: theme.monoFont, fontSize: '11px', fontWeight: 'bold', marginLeft: '12px' }}>
                SAVE CSV
              </button>

              {/* Hidden file input for loading configs */}
              <input 
                type="file" 
                ref={fileInputRef} 
                style={{ display: 'none' }} 
                accept=".json" 
                onChange={handleFileChange} 
              />
            </div>

            {/* Right side: Compile & Plot */}
            <div style={{ display: 'flex', gap: '12px' }}>
              <button onClick={handleCompile} style={{ padding: '6px 16px', backgroundColor: theme.accent, color: '#fff', border: 'none', borderRadius: theme.radius, cursor: 'pointer', fontFamily: theme.monoFont, fontWeight: 'bold' }}>
                COMPILE & DEPLOY
              </button>
              
              <button onClick={onGoToPlot} style={{ padding: '6px 16px', backgroundColor: theme.success, color: '#fff', border: 'none', borderRadius: theme.radius, cursor: 'pointer', fontFamily: theme.monoFont, fontWeight: 'bold' }}>
                VIEW LIVE PLOT
              </button>
            </div>
          </div>

          <div style={{ display: 'flex', flex: 1, overflow: 'hidden' }}>
            
            <Sidebar theme={theme} isDark={isDarkMode} registry={registry} domain="chip" title="RP2040 Chip" alignment="left" />

            <div style={{ flex: 1, position: 'relative' }} ref={reactFlowWrapper}>
              
              <div style={{ position: 'absolute', inset: 0, display: 'flex', pointerEvents: 'none', zIndex: 0 }}>
                <div style={{ flex: 1, borderRight: `2px dashed ${theme.border}`, backgroundColor: 'rgba(59, 130, 246, 0.02)' }}>
                  <div style={{ position: 'absolute', bottom: 20, left: 20, color: theme.border, fontFamily: theme.monoFont, fontSize: '24px', fontWeight: 'bold', opacity: 0.5 }}>
                    HARDWARE (PICO)
                  </div>
                </div>
                <div style={{ flex: 1, backgroundColor: 'rgba(147, 51, 234, 0.02)' }}>
                  <div style={{ position: 'absolute', bottom: 20, right: 20, color: theme.border, fontFamily: theme.monoFont, fontSize: '24px', fontWeight: 'bold', opacity: 0.5 }}>
                    HOST PC (PYTHON)
                  </div>
                </div>
              </div>

              <ReactFlowProvider>
                <ReactFlow nodes={nodes} edges={edges} onNodesChange={onNodesChange} onEdgesChange={onEdgesChange} onConnect={onConnect} onInit={setReactFlowInstance} onDrop={onDrop} onDragOver={e => e.preventDefault()} nodeTypes={nodeTypes} fitView>
                  <Background variant="dots" color={theme.borderDark} gap={20} size={1} />
                </ReactFlow>
              </ReactFlowProvider>
            </div>

            <Sidebar theme={theme} isDark={isDarkMode} registry={registry} domain="pc" title="Mac OS (Host)" alignment="right" />

          </div>
        </div>
      </DarkModeContext.Provider>
    </ThemeContext.Provider>
  );
};

export default ExperimentBuilder;