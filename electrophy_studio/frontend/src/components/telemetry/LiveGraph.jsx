import React, { useState, useEffect, useRef } from 'react';
import Plotly from 'plotly.js-dist-min';

const LiveGraph = ({ theme, isDarkMode }) => {
  const [isConnected, setIsConnected] = useState(false);
  
  // NEW: State to control the Y-Axis lock
  const [yRange, setYRange] = useState('20'); 

  const containerRef = useRef(null);
  const ws = useRef(null);
  
  const plotRefs = useRef({}); 
  const dataBuffer = useRef({}); 
  const xCounter = useRef(0);

  const MAX_POINTS = 300;
  const traceColors = ['#8b5cf6', '#10b981', '#f59e0b', '#3b82f6'];

  // NEW: Dynamically update the layout of existing plots when the dropdown changes
  useEffect(() => {
    Object.values(plotRefs.current).forEach(pState => {
      if (pState.initialized && pState.type === 'time') {
        if (yRange === 'auto') {
          Plotly.relayout(pState.div, { 'yaxis.autorange': true });
        } else {
          const limit = Number(yRange);
          Plotly.relayout(pState.div, { 'yaxis.range': [-limit, limit] });
        }
      }
    });
  }, [yRange]);

  useEffect(() => {
    if (!theme) return;

    const plotBg = isDarkMode ? theme.bg : '#f9fafb';
    const paperBg = isDarkMode ? theme.panelBg : '#ffffff';
    const gridColor = isDarkMode ? theme.border : '#374151'; 
    const axisColor = isDarkMode ? theme.textMuted : '#9ca3af';

    const defaultLayout = {
      autosize: true, margin: { l: 40, r: 16, t: 30, b: 30 },
      plot_bgcolor: plotBg, paper_bgcolor: paperBg,
      font: { color: axisColor, family: theme.monoFont, size: 10 },
      xaxis: { gridcolor: gridColor, zerolinecolor: gridColor, showgrid: true },
      yaxis: { gridcolor: gridColor, zerolinecolor: gridColor, showgrid: true },
      showlegend: false
    };

    ws.current = new WebSocket('ws://localhost:8000/ws/stream');
    ws.current.onopen = () => setIsConnected(true);
    ws.current.onclose = () => setIsConnected(false);

    ws.current.onmessage = (event) => {
      try {
        const payload = JSON.parse(event.data);

        Object.keys(payload).forEach(plotId => {
          const pData = payload[plotId];

          if (!dataBuffer.current[plotId]) {
            dataBuffer.current[plotId] = { type: pData.type, pendingX: [], pendingY: [], latestFreq: null, title: pData.title };
          }

          if (pData.type === 'time') {
            const currentX = xCounter.current++;
            dataBuffer.current[plotId].pendingX.push(pData.data.map(() => currentX));
            dataBuffer.current[plotId].pendingY.push(pData.data);
          } else if (pData.type === 'freq') {
            dataBuffer.current[plotId].latestFreq = pData.data; 
          }
        });
      } catch (err) {
        // Ignore bad frames
      }
    };

    let animationFrameId;
    const renderLoop = () => {
      Object.keys(dataBuffer.current).forEach(plotId => {
        const buffer = dataBuffer.current[plotId];

        // Inside renderLoop(), replace the block that creates the DIV:
        if (!plotRefs.current[plotId]) {
          const newDiv = document.createElement('div');
          // SMART GRID SIZING:
          newDiv.style.gridColumn = buffer.type === 'freq' ? '1 / -1' : 'auto'; // FFT goes full width, Time goes 50%
          newDiv.style.height = '280px'; // Lock the height so it doesn't bounce
          
          newDiv.style.border = `1px solid ${theme.border}`;
          newDiv.style.borderRadius = theme.radius;
          newDiv.style.overflow = 'hidden';
          newDiv.style.backgroundColor = isDarkMode ? theme.panelBg : '#ffffff';
          
          containerRef.current.appendChild(newDiv);
          plotRefs.current[plotId] = { div: newDiv, initialized: false, type: buffer.type };
        }

        const pState = plotRefs.current[plotId];

        // TIME SERIES
        if (buffer.type === 'time' && buffer.pendingY.length > 0) {
          if (!pState.initialized) {
            const traces = buffer.pendingY[0].map((_, i) => ({
              x: [], y: [], type: 'scatter', mode: 'lines',
              line: { color: traceColors[i % traceColors.length], width: 1.5 },
              hoverinfo: 'none' 
            }));

            // Apply the initial Y-Axis config based on the dropdown state
            const yAxisConfig = yRange === 'auto' 
              ? { ...defaultLayout.yaxis, autorange: true } 
              : { ...defaultLayout.yaxis, range: [-Number(yRange), Number(yRange)] };

            Plotly.newPlot(pState.div, traces, { ...defaultLayout, title: buffer.title, yaxis: yAxisConfig }, { staticPlot: true }); 
            pState.initialized = true;
          } else {
            const numTraces = buffer.pendingY[0].length;
            const updateX = Array.from({length: numTraces}, () => []);
            const updateY = Array.from({length: numTraces}, () => []);
            
            buffer.pendingY.forEach((valArray, rowIndex) => {
              valArray.forEach((val, traceIndex) => {
                updateX[traceIndex].push(buffer.pendingX[rowIndex][0]);
                updateY[traceIndex].push(val);
              });
            });

            const traceIndices = Array.from({length: numTraces}, (_, i) => i);
            Plotly.extendTraces(pState.div, { x: updateX, y: updateY }, traceIndices, MAX_POINTS);
          }
          
          buffer.pendingX = [];
          buffer.pendingY = [];
        }

        // FREQUENCY (FFT)
        if (buffer.type === 'freq' && buffer.latestFreq) {
          const freqs = Array.from({length: buffer.latestFreq.length}, (_, i) => i);
          const trace = { x: freqs, y: buffer.latestFreq, type: 'bar', marker: { color: theme.accent }, hoverinfo: 'none' };
          
          // FFT ranges are usually best left on auto or fixed to a specific positive bound, we'll fix the Y-axis to 50 for stability
          const fftLayout = { ...defaultLayout, title: buffer.title, yaxis: { ...defaultLayout.yaxis, range: [0, 50] } };

          if (!pState.initialized) {
            Plotly.newPlot(pState.div, [trace], fftLayout, { staticPlot: true });
            pState.initialized = true;
          } else {
            Plotly.react(pState.div, [trace], fftLayout, { staticPlot: true });
          }
          
          buffer.latestFreq = null; 
        }
      });

      animationFrameId = requestAnimationFrame(renderLoop);
    };

    renderLoop();

    return () => {
      cancelAnimationFrame(animationFrameId);
      if (ws.current) ws.current.close();
      Object.values(plotRefs.current).forEach(p => Plotly.purge(p.div));
    };
  // We explicitly DO NOT include yRange in this dependency array so the websocket doesn't reconnect when changing scale!
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [theme, isDarkMode]);

  if (!theme) return null;

return (
    <div style={{ width: '100%', height: '100%', display: 'flex', flexDirection: 'column', gap: '10px' }}>
      
      {/* HEADER WITH DROPDOWN */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '8px 12px', backgroundColor: theme.panelHeader, border: `1px solid ${theme.border}`, borderRadius: theme.radius, fontFamily: theme.monoFont, fontSize: '12px' }}>
        <div>
          <span style={{ color: isConnected ? theme.success : theme.danger }}>{isConnected ? 'STREAM LIVE' : 'NO SIGNAL'}</span>
          <span style={{ color: theme.textMuted, marginLeft: '16px' }}>60 FPS HARDWARE ACCELERATED</span>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
          <span style={{ color: theme.textMuted }}>TIME Y-AXIS:</span>
          <select 
            value={yRange} 
            onChange={(e) => setYRange(e.target.value)}
            style={{ backgroundColor: theme.bg, color: theme.textMain, border: `1px solid ${theme.border}`, padding: '4px 8px', borderRadius: '4px', outline: 'none', fontFamily: theme.monoFont, fontSize: '11px', cursor: 'pointer' }}
          >
            <option value="auto">Auto-Scale</option>
            <option value="2">± 2</option>
            <option value="10">± 10</option>
            <option value="20">± 20 (IMU)</option>
            <option value="50">± 50</option>
            <option value="100">± 100</option>
            <option value="1000">± 1000 (ToF)</option>
          </select>
        </div>
      </div>
      
      {/* THE NEW SMART GRID CONTAINER */}
      <div 
        ref={containerRef} 
        style={{ 
          flex: 1, 
          display: 'grid', 
          gridTemplateColumns: 'repeat(auto-fit, minmax(45%, 1fr))', // Fits two graphs side-by-side perfectly
          alignContent: 'start',
          gap: '12px', 
          overflowY: 'auto',
          paddingBottom: '20px'
        }} 
      />
    </div>
  );
};

export default LiveGraph;