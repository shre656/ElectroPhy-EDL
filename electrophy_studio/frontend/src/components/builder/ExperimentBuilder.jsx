import React, { useState, useEffect, useRef, useCallback } from 'react';
import ReactFlow, { ReactFlowProvider, useNodesState, useEdgesState, addEdge, Background } from 'reactflow';
import 'reactflow/dist/style.css';
import Sidebar from './Sidebar';
import Toolbar from './Toolbar';
import { nodeTypes, ThemeContext, DarkModeContext } from './CustomNodes';

// Unique ID generator
let globalNodeId = 0;
const getId = () => `node_${Date.now()}_${globalNodeId++}`;

const ExperimentBuilder = ({ theme, isDarkMode, onGoToPlot }) => {
  const reactFlowWrapper = useRef(null);
  const [nodes, setNodes, onNodesChange] = useNodesState([]);
  const [edges, setEdges, onEdgesChange] = useEdgesState([]);
  const [reactFlowInstance, setReactFlowInstance] = useState(null);
  const [registry, setRegistry] = useState(null);
  const [experimentName, setExperimentName] = useState("");

  // 1. Fetch Registry
  useEffect(() => {
    fetch('http://localhost:8000/api/blocks/registry')
      .then(res => res.json())
      .then(data => {
        const grouped = {};
        const typeMapping = { sensor: 'sensorNode', logic: 'logicNode', math: 'mathNode', action: 'actionNode' };
        const colors = { chip: '#3b82f6', pc: '#9333ea' };

        Object.values(data).forEach(block => {
          if (!grouped[block.category]) grouped[block.category] = [];
          grouped[block.category].push({
            ...block,
            color: colors[block.domain] || '#ccc',
            type: typeMapping[block.category] || 'actionNode'
          });
        });
        setRegistry(grouped);
      }).catch(err => console.error("Failed to load blocks", err));
  }, []);

  // 2. React Flow Callbacks
  const onConnect = useCallback((params) => setEdges(eds => addEdge({ ...params, style: { stroke: theme?.accent, strokeWidth: 2 } }, eds)), [setEdges, theme]);

  const onDrop = useCallback((event) => {
    event.preventDefault();
    const rawData = event.dataTransfer.getData('application/reactflow');
    if (!rawData || !reactFlowInstance) return;

    const nodeTemplate = JSON.parse(rawData);
    const bounds = reactFlowWrapper.current.getBoundingClientRect();
    const position = reactFlowInstance.project({ x: event.clientX - bounds.left, y: event.clientY - bounds.top });

    setNodes(nds => nds.concat({
      id: getId(), type: nodeTemplate.type, position,
      data: { ...nodeTemplate, operator: '>', threshold: '' }
    }));
  }, [reactFlowInstance, setNodes]);

  // 3. Toolbar Handlers
  const handleSaveFlow = () => {
    const flowData = { nodes, edges };
    const blob = new Blob([JSON.stringify(flowData, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    
    const link = document.createElement('a');
    link.href = url;
    link.download = experimentName ? `${experimentName}_config.json` : 'experiment_config.json';
    document.body.appendChild(link);
    link.click();
    
    // Cleanup
    document.body.removeChild(link);
    URL.revokeObjectURL(url);
  };
  
  const handleLoadFlow = (file) => {
    const reader = new FileReader();
    reader.onload = (e) => {
      const flowData = JSON.parse(e.target.result);
      if (flowData.nodes && flowData.edges) {
        setNodes(flowData.nodes);
        setEdges(flowData.edges);
        setExperimentName(file.name.replace(/(_config)?\.json$/, ''));
      }
    };
    reader.readAsText(file);
  };

  const handleSaveData = async () => {
    try {
      const response = await fetch('http://localhost:8000/api/experiment/save', {
        method: 'POST',
      });
      const data = await response.json();
      
      if (response.ok && data.status === 'saved') {
        alert(`Data saved successfully to ${data.file} on the host machine.`);
      } else {
        alert(`Error saving data: ${data.message || 'Unknown error'}`);
      }
    } catch (error) {
      console.error("Failed to save data:", error);
      alert("Failed to communicate with the backend to save data.");
    }
  };

  const handleCompile = async () => {
    try {
      const response = await fetch('http://localhost:8000/api/experiment/compile', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ nodes, edges }),
      });

      if (!response.ok) {
        const errorData = await response.json();
        throw new Error(errorData.detail || 'Compilation failed on the backend.');
      }

      const data = await response.json();
      console.log(`Success! Compiled and sent ${data.bytes_sent} bytes to hardware.`);
      
      // Automatically transition to the plotting view if compilation succeeds
      if (onGoToPlot) {
        onGoToPlot();
      }
      
    } catch (error) {
      console.error("Compile Error:", error);
      alert(`Compile Error: ${error.message}`);
    }
  };

  // --- MISSING RENDER BLOCK RESTORED HERE ---
  if (!theme) return null;

  return (
    <ThemeContext.Provider value={theme}>
      <DarkModeContext.Provider value={isDarkMode}>
        <div style={{ display: 'flex', flexDirection: 'column', height: '100%', width: '100%' }}>
          
          <Toolbar 
            theme={theme}
            experimentName={experimentName} setExperimentName={setExperimentName}
            onSaveFlow={handleSaveFlow} onLoadFlow={handleLoadFlow}
            onSaveData={handleSaveData} onCompile={handleCompile} onGoToPlot={onGoToPlot}
          />

          <div style={{ display: 'flex', flex: 1, overflow: 'hidden' }}>
            <Sidebar theme={theme} isDark={isDarkMode} registry={registry} domain="chip" title="RP2040 Chip" alignment="left" />

            <div style={{ flex: 1, position: 'relative' }} ref={reactFlowWrapper}>
              <ReactFlowProvider>
                <ReactFlow 
                  nodes={nodes} edges={edges} 
                  onNodesChange={onNodesChange} onEdgesChange={onEdgesChange} 
                  onConnect={onConnect} onInit={setReactFlowInstance} 
                  onDrop={onDrop} onDragOver={e => e.preventDefault()} 
                  nodeTypes={nodeTypes} fitView
                >
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