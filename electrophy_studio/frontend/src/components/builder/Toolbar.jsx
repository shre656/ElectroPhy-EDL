import React, { useRef } from 'react';

const Toolbar = ({ 
  theme, 
  experimentName, 
  setExperimentName, 
  onSaveFlow, 
  onLoadFlow, 
  onSaveData, 
  onCompile, 
  onGoToPlot 
}) => {
  const fileInputRef = useRef(null);

  const handleFileChange = (e) => {
    const file = e.target.files[0];
    if (!file) return;
    onLoadFlow(file);
    e.target.value = null; // Reset input
  };

  const btnStyle = {
    padding: '6px 12px',
    backgroundColor: theme.panelBg,
    color: theme.textMain,
    border: `1px solid ${theme.border}`,
    borderRadius: theme.radius,
    cursor: 'pointer',
    fontFamily: theme.monoFont,
    fontSize: '11px',
    fontWeight: 'bold'
  };

  return (
    <div style={{ padding: '8px 16px', backgroundColor: theme.panelHeader, borderBottom: `1px solid ${theme.border}`, display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
      
      {/* Flow & Data Management */}
      <div style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
        <input 
          type="text" 
          placeholder="Experiment Name..." 
          value={experimentName}
          onChange={(e) => setExperimentName(e.target.value)}
          style={{ padding: '6px 10px', backgroundColor: theme.bg, border: `1px solid ${theme.border}`, color: theme.textMain, fontFamily: theme.monoFont, fontSize: '12px', borderRadius: theme.radius, width: '200px', outline: 'none' }}
        />
        <button onClick={onSaveFlow} style={btnStyle}>SAVE FLOW</button>
        <button onClick={() => fileInputRef.current.click()} style={btnStyle}>LOAD FLOW</button>
        <button onClick={onSaveData} style={{ ...btnStyle, marginLeft: '12px' }}>SAVE CSV</button>
        
        <input type="file" ref={fileInputRef} style={{ display: 'none' }} accept=".json" onChange={handleFileChange} />
      </div>

      {/* Compile & Plot */}
      <div style={{ display: 'flex', gap: '12px' }}>
        <button onClick={onCompile} style={{ ...btnStyle, backgroundColor: theme.accent, color: '#fff', border: 'none' }}>
          COMPILE & DEPLOY
        </button>
        <button onClick={onGoToPlot} style={{ ...btnStyle, backgroundColor: theme.success, color: '#fff', border: 'none' }}>
          VIEW LIVE PLOT
        </button>
      </div>
    </div>
  );
};

export default Toolbar;