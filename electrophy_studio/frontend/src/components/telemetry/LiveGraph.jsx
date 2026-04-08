import React, { useState, useEffect, useRef } from 'react';
import Plotly from 'plotly.js-dist-min';

const LiveGraph = ({ theme, isDarkMode }) => {
  const [isConnected, setIsConnected] = useState(false);
  const [yRange, setYRange] = useState('20');
  const [isStreaming, setIsStreaming] = useState(false); 
  const [plotConfigs, setPlotConfigs] = useState({}); 

  const ws = useRef(null);
  const plotRefs = useRef({}); 
  const dataBuffer = useRef({}); 
  const xCounter = useRef(0);
  const animationFrameId = useRef(null);

  const MAX_POINTS = 300;
  const traceColors = ['#8b5cf6', '#10b981', '#f59e0b', '#3b82f6'];

  const startStream = () => fetch('http://localhost:8000/api/stream/start', { method: 'POST' })
    .then(() => setIsStreaming(true))
    .catch(err => console.error("Failed to start stream", err));
    
  const stopStream  = () => fetch('http://localhost:8000/api/stream/stop',  { method: 'POST' })
    .then(() => setIsStreaming(false))
    .catch(err => console.error("Failed to stop stream", err));

  useEffect(() => {
    Object.keys(plotRefs.current).forEach(id => {
      const pState = plotRefs.current[id];
      if (pState && pState.type === 'time') {
        const layoutUpdate = yRange === 'auto' 
          ? { 'yaxis.autorange': true } 
          : { 'yaxis.range': [-Number(yRange), Number(yRange)] };
        Plotly.relayout(`plot-${id}`, layoutUpdate);
      }
    });
  }, [yRange]);

  useEffect(() => {
    if (!theme) return;

    const defaultLayout = {
      autosize: true, margin: { l: 40, r: 16, t: 30, b: 30 },
      plot_bgcolor: isDarkMode ? theme.bg : '#f9fafb',
      paper_bgcolor: isDarkMode ? theme.panelBg : '#ffffff',
      font: { color: isDarkMode ? theme.textMuted : '#9ca3af', family: theme.monoFont, size: 10 },
      xaxis: { gridcolor: isDarkMode ? theme.border : '#374151', showgrid: true },
      yaxis: { gridcolor: isDarkMode ? theme.border : '#374151', showgrid: true },
      showlegend: false
    };

    ws.current = new WebSocket('ws://localhost:8000/ws/stream');
    ws.current.onopen = () => setIsConnected(true);
    ws.current.onclose = () => setIsConnected(false);

    ws.current.onmessage = (event) => {
      try {
        const payload = JSON.parse(event.data);
        let hasNewPlots = false;

        Object.keys(payload).forEach(plotId => {
          const pData = payload[plotId];

          if (!dataBuffer.current[plotId]) {
            dataBuffer.current[plotId] = { type: pData.type, pendingX: [], pendingY: [], latestFreq: null, title: pData.title };
            plotRefs.current[plotId] = { initialized: false, type: pData.type };
            hasNewPlots = true;
          }

          if (pData.type === 'time') {
            // FIXED: X-axis counter bug
            const currentX = xCounter.current++;
            dataBuffer.current[plotId].pendingX.push(currentX);
            dataBuffer.current[plotId].pendingY.push(pData.data);
          } else if (pData.type === 'freq') {
            dataBuffer.current[plotId].latestFreq = pData.data; 
          }
        });

        if (hasNewPlots) {
          setPlotConfigs(prev => {
            const next = { ...prev };
            Object.keys(payload).forEach(id => {
              if (!next[id]) next[id] = { type: payload[id].type, title: payload[id].title };
            });
            return next;
          });
        }
      } catch (err) { /* Ignore bad frames */ }
    };

    const renderLoop = () => {
      Object.keys(dataBuffer.current).forEach(plotId => {
        const buffer = dataBuffer.current[plotId];
        const pState = plotRefs.current[plotId];
        const divId = `plot-${plotId}`;
        
        const graphDiv = document.getElementById(divId);
        if (!graphDiv) return;

        // NEW: Try/Catch around Plotly ensures the render loop never dies
        try {
          // --- TIME SERIES ---
          if (buffer.type === 'time' && buffer.pendingY.length > 0) {
            const currentNumTraces = buffer.pendingY[0].length;

            // NEW: Self-Healing Logic! If you unplugged a wire, wipe the graph and restart it.
            if (pState.initialized && graphDiv.data && graphDiv.data.length !== currentNumTraces) {
              Plotly.purge(divId);
              pState.initialized = false;
            }

            if (!pState.initialized) {
              const traces = Array.from({length: currentNumTraces}, (_, i) => ({
                x: [], y: [], type: 'scatter', mode: 'lines',
                line: { color: traceColors[i % traceColors.length], width: 1.5 },
                hoverinfo: 'none' 
              }));
              const yAxis = yRange === 'auto' ? { autorange: true } : { range: [-Number(yRange), Number(yRange)] };
              Plotly.newPlot(divId, traces, { ...defaultLayout, title: buffer.title, yaxis: { ...defaultLayout.yaxis, ...yAxis } }, { staticPlot: true }); 
              pState.initialized = true;
            } else {
              const updateX = Array.from({length: currentNumTraces}, () => []);
              const updateY = Array.from({length: currentNumTraces}, () => []);
              
              buffer.pendingY.forEach((valArray, rowIndex) => {
                // Ignore weird frames during compiler transitions
                if (valArray.length !== currentNumTraces) return; 

                valArray.forEach((val, traceIndex) => {
                  updateX[traceIndex].push(buffer.pendingX[rowIndex]);
                  updateY[traceIndex].push(val);
                });
              });

              if (updateX[0].length > 0) {
                Plotly.extendTraces(divId, { x: updateX, y: updateY }, Array.from({length: currentNumTraces}, (_, i) => i), MAX_POINTS);
              }
            }
            buffer.pendingX = []; buffer.pendingY = [];
          }

          // --- FFT / FREQUENCY ---
          if (buffer.type === 'freq' && buffer.latestFreq) {
            const freqs = Array.from({length: buffer.latestFreq.length}, (_, i) => i);
            const trace = { x: freqs, y: buffer.latestFreq, type: 'bar', marker: { color: theme.accent }, hoverinfo: 'none' };
            const fftLayout = { ...defaultLayout, title: buffer.title, yaxis: { ...defaultLayout.yaxis, range: [0, 50] } };

            if (!pState.initialized) {
              Plotly.newPlot(divId, [trace], fftLayout, { staticPlot: true });
              pState.initialized = true;
            } else {
              Plotly.react(divId, [trace], fftLayout, { staticPlot: true });
            }
            buffer.latestFreq = null; 
          }
        } catch (err) {
           // If Plotly still somehow errors out, flag it to re-initialize next frame
           pState.initialized = false;
           buffer.pendingX = []; buffer.pendingY = [];
        }
      });

      animationFrameId.current = requestAnimationFrame(renderLoop);
    };

    renderLoop();
    startStream();

    return () => {
      cancelAnimationFrame(animationFrameId.current);
      if (ws.current) ws.current.close();
      Object.keys(plotRefs.current).forEach(id => Plotly.purge(`plot-${id}`));
      stopStream();
    };
  }, [theme, isDarkMode]); 

  if (!theme) return null;

  return (
    <div style={{ width: '100%', height: '100%', display: 'flex', flexDirection: 'column', gap: '10px' }}>
      
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '8px 12px', backgroundColor: theme.panelHeader, border: `1px solid ${theme.border}`, borderRadius: theme.radius, fontFamily: theme.monoFont, fontSize: '12px' }}>
        <div>
          <span style={{ color: isConnected ? theme.success : theme.danger }}>{isConnected ? 'STREAM LIVE' : 'NO SIGNAL'}</span>
          <span style={{ color: theme.textMuted, marginLeft: '16px' }}>60 FPS HARDWARE ACCELERATED</span>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
          
          <button 
            onClick={isStreaming ? stopStream : startStream}
            style={{ 
              backgroundColor: isStreaming ? theme.danger : theme.success, 
              color: '#fff', border: 'none', padding: '4px 12px', 
              borderRadius: '4px', cursor: 'pointer', fontFamily: theme.monoFont, fontWeight: 'bold' 
            }}
          >
            {isStreaming ? 'PAUSE STREAM' : 'START STREAM'}
          </button>

          <span style={{ color: theme.textMuted }}>TIME Y-AXIS:</span>
          <select value={yRange} onChange={(e) => setYRange(e.target.value)} style={{ backgroundColor: theme.bg, color: theme.textMain, border: `1px solid ${theme.border}`, padding: '4px 8px', borderRadius: '4px', outline: 'none', fontFamily: theme.monoFont }}>
            <option value="auto">Auto-Scale</option>
            <option value="2">± 2</option>
            <option value="10">± 10</option>
            <option value="20">± 20 (IMU)</option>
            <option value="50">± 50</option>
            <option value="1000">± 1000 (ToF)</option>
          </select>
        </div>
      </div>
      
      <div style={{ flex: 1, display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(45%, 1fr))', alignContent: 'start', gap: '12px', overflowY: 'auto', paddingBottom: '20px' }}>
        {Object.entries(plotConfigs).map(([id, config]) => (
          <div 
            key={id} 
            id={`plot-${id}`} 
            style={{ 
              height: '280px', 
              border: `1px solid ${theme.border}`, 
              borderRadius: theme.radius, 
              overflow: 'hidden', 
              backgroundColor: isDarkMode ? theme.panelBg : '#ffffff',
              gridColumn: config.type === 'freq' ? '1 / -1' : 'auto' 
            }} 
          />
        ))}
      </div>
    </div>
  );
};

export default LiveGraph;