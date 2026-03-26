import React from 'react';

const Dashboard = ({ theme, isDarkMode, onLaunchBuilder, savedExperiments = [] }) => {
  if (!theme) return null;

  return (
    <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', height: '100%', padding: '40px' }}>
      <div style={{ 
        width: '600px', 
        backgroundColor: theme.panelBg, 
        border: `1px solid ${theme.border}`, 
        borderRadius: theme.radius,
        padding: '30px',
        ...(isDarkMode ? {} : { boxShadow: theme.raisedShadow })
      }}>
        
        <h2 style={{ color: theme.textMain, fontFamily: theme.monoFont, marginTop: 0, marginBottom: '24px', borderBottom: `1px solid ${theme.border}`, paddingBottom: '12px' }}>
          EXPERIMENT CONTROL
        </h2>

        {/* Action Button */}
        <button 
          onClick={onLaunchBuilder}
          style={{
            width: '100%', padding: '16px', backgroundColor: theme.accent, color: '#fff',
            border: 'none', borderRadius: theme.radius, cursor: 'pointer',
            fontFamily: theme.monoFont, fontSize: '14px', fontWeight: 'bold', letterSpacing: '1px',
            marginBottom: '30px', transition: 'opacity 0.2s'
          }}
          onMouseOver={e => e.target.style.opacity = 0.8}
          onMouseOut={e => e.target.style.opacity = 1}
        >
          + BUILD NEW EXPERIMENT
        </button>

        {/* Saved Experiments List */}
        <div style={{ fontFamily: theme.monoFont, fontSize: '12px', color: theme.textMuted, marginBottom: '10px', textTransform: 'uppercase' }}>
          Saved Configurations
        </div>
        
        <div style={{ display: 'flex', flexDirection: 'column', gap: '8px' }}>
          {savedExperiments.length === 0 ? (
            <div style={{ padding: '20px', textAlign: 'center', color: theme.textMuted, border: `1px dashed ${theme.border}`, borderRadius: theme.radius }}>
              No saved experiments found.
            </div>
          ) : (
            savedExperiments.map((exp, index) => (
              <div key={index} style={{
                padding: '12px 16px', backgroundColor: theme.bg, border: `1px solid ${theme.border}`,
                borderRadius: theme.radius, display: 'flex', justifyContent: 'space-between', alignItems: 'center',
                cursor: 'pointer'
              }}
              onMouseOver={e => e.currentTarget.style.borderColor = theme.accent}
              onMouseOut={e => e.currentTarget.style.borderColor = theme.border}
              >
                <span style={{ color: theme.textMain, fontFamily: theme.monoFont, fontSize: '13px' }}>{exp.name}</span>
                <span style={{ color: theme.textMuted, fontSize: '11px' }}>LOAD &rarr;</span>
              </div>
            ))
          )}
        </div>

      </div>
    </div>
  );
};

export default Dashboard;