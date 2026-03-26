/* eslint-disable react-refresh/only-export-components */
import React, { createContext, useContext } from 'react';
import { Handle, Position, useReactFlow } from 'reactflow';

export const ThemeContext = createContext(null);
export const DarkModeContext = createContext(true);

// UPGRADE 1: Added 'selected' state to style the container when clicked
const nodeContainerStyle = (color, theme, isDark, selected) => ({
  background: theme.panelBg, 
  border: `1px solid ${selected ? theme.accent : color}`, 
  boxShadow: selected ? `0 0 0 2px ${theme.accent}40` : (isDark ? 'none' : `inset -1px -1px 0 ${theme.borderDark}, inset 1px 1px 0 ${theme.borderLight}`),
  borderRadius: theme.radius,
  minWidth: '180px', color: theme.textMain, fontFamily: theme.uiFont, overflow: 'visible',
  transition: 'all 0.15s ease-in-out'
});

const nodeHeaderStyle = (color, theme) => ({
  background: theme.panelHeader, padding: '6px 10px', borderBottom: `1px solid ${color}`,
  fontWeight: '700', fontSize: '10px', fontFamily: theme.monoFont, textAlign: 'center',
  letterSpacing: '0.8px', color: theme.textMuted, textTransform: 'uppercase',
  position: 'relative' // Needed to position the delete button
});

const handleStyle = (theme) => ({
  background: theme.accent, border: `2px solid ${theme.panelBg}`, width: '10px', height: '10px'
});

// UPGRADE 2: A reusable Delete Button component
const DeleteNodeButton = ({ id }) => {
  const { setNodes, setEdges } = useReactFlow();
  const theme = useContext(ThemeContext);
  
  const handleDelete = (e) => {
    e.stopPropagation(); // Prevents the click from dragging the node
    setNodes((nds) => nds.filter((n) => n.id !== id));
    setEdges((eds) => eds.filter((edge) => edge.source !== id && edge.target !== id));
  };

  return (
    <div 
      onClick={handleDelete}
      style={{
        position: 'absolute', right: '6px', top: '50%', transform: 'translateY(-50%)',
        cursor: 'pointer', color: theme.danger || '#ef4444', fontSize: '9px',
        width: '14px', height: '14px', display: 'flex', alignItems: 'center', justifyContent: 'center',
        borderRadius: '50%', background: theme.bg, border: `1px solid ${theme.border}`,
        fontWeight: 'bold'
      }}
      title="Delete Node"
    >
      ✕
    </div>
  );
};

// --- NODE COMPONENTS (Updated to receive 'id' and 'selected' props) ---

const SensorNode = ({ id, data, selected }) => {
  const theme = useContext(ThemeContext);
  const isDark = useContext(DarkModeContext);
  const outputLabels = data.config?.outputs || ['Data Out'];

  return (
    <div style={nodeContainerStyle(data.color, theme, isDark, selected)}>
      <div style={nodeHeaderStyle(data.color, theme)}>
        {data.label}
        <DeleteNodeButton id={id} />
      </div>
      <div style={{ display: 'flex', flexDirection: 'column', padding: '6px 0' }}>
        {outputLabels.map((lbl, idx) => (
          <div key={idx} style={{ position: 'relative', padding: '4px 12px', textAlign: 'right', fontSize: '10px', fontFamily: theme.monoFont, color: theme.textMain }}>
            {lbl}
            <Handle type="source" position={Position.Right} id={`out_${idx}`} style={{ ...handleStyle(theme), top: '50%', transform: 'translateY(-50%)', right: '-6px' }} />
          </div>
        ))}
      </div>
    </div>
  );
};

const ActionNode = ({ id, data, selected }) => {
  const theme = useContext(ThemeContext);
  const isDark = useContext(DarkModeContext);
  const inputLabels = data.config?.inputs || ['Data In'];

  return (
    <div style={nodeContainerStyle(data.color, theme, isDark, selected)}>
      <div style={nodeHeaderStyle(data.color, theme)}>
        {data.label}
        <DeleteNodeButton id={id} />
      </div>
      <div style={{ display: 'flex', flexDirection: 'column', padding: '6px 0' }}>
        {inputLabels.map((lbl, idx) => (
          <div key={idx} style={{ position: 'relative', padding: '4px 12px', textAlign: 'left', fontSize: '10px', fontFamily: theme.monoFont, color: theme.textMain }}>
            <Handle type="target" position={Position.Left} id={`in_${idx}`} style={{ ...handleStyle(theme), top: '50%', transform: 'translateY(-50%)', left: '-6px' }} />
            {lbl}
          </div>
        ))}
      </div>
      <div style={{ padding: '4px 10px', textAlign: 'center', borderTop: `1px solid ${theme.border}` }}>
        <span style={{ fontFamily: theme.monoFont, fontSize: '9px', color: theme.textMuted, letterSpacing: '0.5px' }}>
          {data.domain === 'chip' ? 'HARDWARE OUT' : 'PC ACTION'}
        </span>
      </div>
    </div>
  );
};

const LogicNode = ({ id, data, selected }) => {
  const { setNodes } = useReactFlow();
  const theme = useContext(ThemeContext);
  const isDark = useContext(DarkModeContext);
  const handleChange = (field, val) => setNodes(nds => nds.map(n => n.id === id ? { ...n, data: { ...n.data, [field]: val } } : n));
  
  return (
    <div style={nodeContainerStyle(data.color, theme, isDark, selected)}>
      <Handle type="target" position={Position.Left} id="in_0" style={{ ...handleStyle(theme), left: '-6px' }} />
      <div style={nodeHeaderStyle(data.color, theme)}>
        {data.label}
        <DeleteNodeButton id={id} />
      </div>
      <div style={{ padding: '8px 10px', display: 'flex', gap: '6px' }}>
        <select onChange={e => handleChange('operator', e.target.value)} defaultValue=">" style={{ width: '45px', textAlign: 'center', backgroundColor: theme.statusBg, color: theme.textMain, border: `1px solid ${theme.border}`, outline: 'none' }}>
          <option value=">">&gt;</option><option value="<">&lt;</option><option value="==">==</option>
        </select>
        <input type="number" placeholder="Value" onChange={e => handleChange('threshold', e.target.value)} style={{ width: '100%', padding: '4px', backgroundColor: theme.statusBg, color: theme.textMain, border: `1px solid ${theme.border}`, outline: 'none' }} />
      </div>
      <Handle type="source" position={Position.Right} id="out_0" style={{ ...handleStyle(theme), right: '-6px' }} />
    </div>
  );
};

const MathNode = ({ id, data, selected }) => {
  const { setNodes } = useReactFlow();
  const theme = useContext(ThemeContext);
  const isDark = useContext(DarkModeContext);

  const inputLabels = data.config?.inputs;
  const outputLabels = data.config?.outputs || ['Out'];

  // Upgraded input styling
  const inputStyle = {
    width: '100%',
    padding: '6px',
    textAlign: 'center',
    backgroundColor: isDark ? 'rgba(0, 0, 0, 0.2)' : '#ffffff',
    color: theme.textMain,
    border: `1px solid ${theme.border}`,
    borderRadius: '4px',
    outline: 'none',
    fontFamily: theme.monoFont,
    fontSize: '12px'
  };

  return (
    <div style={nodeContainerStyle(data.color, theme, isDark, selected)}>
      <div style={nodeHeaderStyle(data.color, theme)}>
        {data.label}
        <DeleteNodeButton id={id} />
      </div>

      {inputLabels ? (
        <div style={{ display: 'flex', justifyContent: 'space-between', padding: '6px 0' }}>
          <div style={{ display: 'flex', flexDirection: 'column' }}>
            {inputLabels.map((lbl, idx) => (
              <div key={`in_${idx}`} style={{ position: 'relative', padding: '4px 12px', textAlign: 'left', fontSize: '10px', fontFamily: theme.monoFont, color: theme.textMain }}>
                <Handle type="target" position={Position.Left} id={`in_${idx}`} style={{ ...handleStyle(theme), top: '50%', transform: 'translateY(-50%)', left: '-6px' }} />
                {lbl}
              </div>
            ))}
          </div>
          <div style={{ display: 'flex', flexDirection: 'column' }}>
            {outputLabels.map((lbl, idx) => (
              <div key={`out_${idx}`} style={{ position: 'relative', padding: '4px 12px', textAlign: 'right', fontSize: '10px', fontFamily: theme.monoFont, color: theme.textMain }}>
                {lbl}
                <Handle type="source" position={Position.Right} id={`out_${idx}`} style={{ ...handleStyle(theme), top: '50%', transform: 'translateY(-50%)', right: '-6px' }} />
              </div>
            ))}
          </div>
        </div>
      ) : (
        <>
          <Handle type="target" position={Position.Left} id="in_0" style={{ ...handleStyle(theme), left: '-6px' }} />
          <div style={{ padding: '8px 10px' }}>
            <input 
              type="number" 
              placeholder="Value..." 
              defaultValue={data.threshold || ''}
              onChange={e => setNodes(nds => nds.map(n => n.id === id ? { ...n, data: { ...n.data, threshold: e.target.value } } : n))} 
              style={inputStyle} 
            />
          </div>
          <Handle type="source" position={Position.Right} id="out_0" style={{ ...handleStyle(theme), right: '-6px' }} />
        </>
      )}
    </div>
  );
};

export const nodeTypes = { sensorNode: SensorNode, logicNode: LogicNode, mathNode: MathNode, actionNode: ActionNode };