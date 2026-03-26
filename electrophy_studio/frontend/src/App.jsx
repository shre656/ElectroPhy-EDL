import React, { useState } from 'react';
import Dashboard from './components/dashboard/Dashboard';
import ExperimentBuilder from './components/builder/ExperimentBuilder';
import LiveGraph from './components/telemetry/LiveGraph';

// Restored Global Themes
const THEMES = {
  dark: {
    bg: '#111111', panelBg: '#1a1a1a', panelHeader: '#222222', border: '#333',
    borderLight: '#444', borderDark: '#0a0a0a', textMain: '#eeeeee', textMuted: '#888888',
    accent: '#8b5cf6', success: '#10b981', danger: '#ef4444', radius: '6px',
    monoFont: '"JetBrains Mono", "Fira Code", monospace', uiFont: 'Inter, system-ui, sans-serif'
  },
  light: {
    bg: '#f9fafb', panelBg: '#ffffff', panelHeader: '#f3f4f6', border: '#e5e7eb',
    borderLight: '#ffffff', borderDark: '#d1d5db', textMain: '#111827', textMuted: '#6b7280',
    accent: '#7c3aed', success: '#059669', danger: '#dc2626', radius: '6px',
    monoFont: '"JetBrains Mono", "Fira Code", monospace', uiFont: 'Inter, system-ui, sans-serif'
  }
};

const App = () => {
  const [currentView, setCurrentView] = useState('dashboard'); // 'dashboard', 'builder', 'telemetry'
  const [isDarkMode, setIsDarkMode] = useState(true);
  const theme = isDarkMode ? THEMES.dark : THEMES.light;

  // --- Hardware Connection State ---
  const [isConnected, setIsConnected] = useState(false);
  const [portName, setPortName] = useState('');

  const handleConnect = async () => {
    try {
      const res = await fetch('http://localhost:8000/api/hardware/connect');
      const data = await res.json();
      if (data.status === 'connected' || data.status === 'already connected') {
        setIsConnected(true);
        setPortName(data.port || 'USB');
      } else {
        alert('Hardware Connection Failed: ' + data.error);
      }
    } catch (err) {
      alert('Backend unreachable. Is the Python server running?');
    }
  };

  const savedExps = [
    { name: "BNO055_Basic_Telemetry" },
    { name: "ToF_Distance_Threshold" }
  ];

  return (
    <div style={{ height: '100%', width: '100%', backgroundColor: theme.bg, display: 'flex', flexDirection: 'column', color: theme.textMain, fontFamily: theme.uiFont }}>
      
      {/* Universal Top Nav - Restored to exact original styling */}
      <div style={{ padding: '12px 20px', backgroundColor: theme.panelHeader, borderBottom: `1px solid ${theme.border}`, display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        
        <div style={{ fontFamily: theme.monoFont, fontWeight: 'bold', fontSize: '16px', cursor: 'pointer' }} onClick={() => setCurrentView('dashboard')}>
          ElectroPhy_Studio <span style={{ color: theme.textMuted, fontSize: '12px' }}>v1.0.6</span>
        </div>
        
        <div style={{ display: 'flex', alignItems: 'center', gap: '12px' }}>
          
          {/* Connection Status Badge */}
          <div style={{ 
            display: 'flex', alignItems: 'center', gap: '8px', padding: '6px 12px',
            backgroundColor: isDarkMode ? '#1a1a1a' : '#e5e7eb', 
            border: `1px solid ${theme.border}`, borderRadius: theme.radius,
            fontFamily: theme.monoFont, fontSize: '11px', color: theme.textMuted
          }}>
            <div style={{
              width: '8px', height: '8px', borderRadius: '50%',
              backgroundColor: isConnected ? theme.success : theme.borderLight,
              boxShadow: isConnected ? `0 0 6px ${theme.success}` : 'none'
            }} />
            {isConnected ? `Connected [${portName}]` : 'Disconnected'}
          </div>

          {/* Theme Toggle */}
          <button onClick={() => setIsDarkMode(!isDarkMode)} style={{ 
            padding: '6px 12px', backgroundColor: isDarkMode ? '#1a1a1a' : '#e5e7eb', color: theme.textMain, 
            border: `1px solid ${theme.border}`, borderRadius: theme.radius, cursor: 'pointer', 
            fontFamily: theme.monoFont, fontSize: '11px', fontWeight: 'bold'
          }}>
            {isDarkMode ? 'LIGHT MODE' : 'DARK MODE'}
          </button>

          {/* INIT COMM / Dashboard Navigation */}
          {!isConnected ? (
            <button onClick={handleConnect} style={{ 
              padding: '6px 16px', backgroundColor: '#d97757', color: '#fff', 
              border: 'none', borderRadius: theme.radius, cursor: 'pointer', 
              fontFamily: theme.monoFont, fontSize: '11px', fontWeight: 'bold', letterSpacing: '0.5px'
            }}>
              INIT COMM
            </button>
          ) : (
             currentView !== 'dashboard' && (
              <button onClick={() => setCurrentView('dashboard')} style={{ 
                padding: '6px 12px', backgroundColor: 'transparent', color: theme.textMuted, 
                border: `1px solid ${theme.border}`, borderRadius: theme.radius, cursor: 'pointer', 
                fontFamily: theme.monoFont, fontSize: '11px' 
              }}>
                Return to Dashboard
              </button>
            )
          )}
        </div>
      </div>

      {/* Main Content Router */}
      <div style={{ flex: 1, position: 'relative', overflow: 'hidden' }}>
        
        {/* Dashboard handles its own mounting/unmounting */}
        {currentView === 'dashboard' && (
          <Dashboard theme={theme} isDarkMode={isDarkMode} savedExperiments={savedExps} onLaunchBuilder={() => setCurrentView('builder')} />
        )}
        
        {/* FIX: Builder and Telemetry are ALWAYS mounted. We just toggle their visibility with CSS. */}
        {/* This prevents ReactFlow from destroying your nodes when you switch views. */}
        <div style={{ display: currentView === 'builder' ? 'flex' : 'none', height: '100%', width: '100%' }}>
          <ExperimentBuilder theme={theme} isDarkMode={isDarkMode} onGoToPlot={() => setCurrentView('telemetry')} />
        </div>

        <div style={{ display: currentView === 'telemetry' ? 'flex' : 'none', height: '100%', width: '100%', flexDirection: 'column', padding: '20px' }}>
          <div style={{ marginBottom: '10px' }}>
            <button onClick={() => setCurrentView('builder')} style={{ padding: '8px 16px', backgroundColor: theme.panelBg, color: theme.textMain, border: `1px solid ${theme.border}`, borderRadius: theme.radius, cursor: 'pointer', fontFamily: theme.monoFont }}>
              &larr; Back to Canvas
            </button>
          </div>
          <LiveGraph theme={theme} isDarkMode={isDarkMode} />
        </div>

      </div>
    </div>
  );
};

export default App;