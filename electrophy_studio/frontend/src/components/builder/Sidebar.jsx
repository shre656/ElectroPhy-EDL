import React from 'react';

const Sidebar = ({ theme, isDark, registry, domain, title, alignment }) => {
  if (!registry) return null;

  // Filter blocks based on whether they belong to the Chip or the PC
  const domainBlocks = {};
  Object.entries(registry).forEach(([category, blocks]) => {
    const filtered = blocks.filter(b => b.domain === domain);
    if (filtered.length > 0) domainBlocks[category] = filtered;
  });

  return (
    <div style={{
      width: '200px', backgroundColor: theme.panelBg, overflowY: 'auto', flexShrink: 0,
      borderRight: alignment === 'left' ? `1px solid ${theme.border}` : 'none',
      borderLeft: alignment === 'right' ? `1px solid ${theme.border}` : 'none',
    }}>
      <div style={{
        padding: '12px 16px', fontFamily: theme.monoFont, fontSize: '11px', fontWeight: 'bold',
        color: domain === 'chip' ? '#3b82f6' : '#9333ea', letterSpacing: '1px',
        borderBottom: `1px solid ${theme.border}`, textTransform: 'uppercase', textAlign: 'center'
      }}>
        {title}
      </div>
      {Object.entries(domainBlocks).map(([category, blocks]) => (
        <React.Fragment key={category}>
          <div style={{ padding: '10px 14px 4px', fontSize: '9px', fontFamily: theme.monoFont, color: theme.textMuted, textTransform: 'uppercase', fontWeight: 'bold' }}>
            {category}
          </div>
          {blocks.map(node => (
            <div key={node.id} draggable onDragStart={e => { e.dataTransfer.setData('application/reactflow', JSON.stringify(node)); e.dataTransfer.effectAllowed = 'move'; }}
              style={{
                margin: '4px 10px', padding: '8px 10px', borderLeft: `3px solid ${node.color}`, borderTop: `1px solid ${theme.border}`, borderRight: `1px solid ${theme.border}`, borderBottom: `1px solid ${theme.border}`,
                borderRadius: theme.radius, cursor: 'grab', backgroundColor: theme.bg, color: theme.textMain, fontFamily: theme.monoFont, fontSize: '11px', fontWeight: '500',
                ...(isDark ? {} : { boxShadow: theme.raisedShadow })
              }}>
              {node.label}
            </div>
          ))}
        </React.Fragment>
      ))}
    </div>
  );
};

export default Sidebar;