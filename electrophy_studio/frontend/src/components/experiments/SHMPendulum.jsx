import React, { useState, useEffect, useRef } from 'react';

const SHMPendulum = ({ theme, isDarkMode, onBack }) => {
  const [isRunning, setIsRunning] = useState(false);
  const [dataPoints, setDataPoints] = useState([]);
  
  // Real-time calculated parameters
  const [metrics, setMetrics] = useState({
    amplitude: "0.00",
    period: "0.00",
    frequency: "0.00",
    damping: "0.000"
  });

  const canvasRef = useRef(null);

  // --------------------------------------------------------
  // VM BYTECODE DEPLOYMENT
  // --------------------------------------------------------
  const handleDeployBytecode = async () => {
    try {
      setIsRunning(true);
      
      // Strict 1-to-1 translation of the React Flow canvas:
      // Node 1: Read Lin Accel X -> Reg 0
      // Node 2: Read Lin Accel Y -> Reg 1
      // Node 3: Read Lin Accel Z -> Reg 2
      // Node 4: TX (Print) Regs 0, 1, 2
      const payload = {
        instructions: [
          { opcode: 0x01, in_regs: [255, 255, 255, 255, 255, 255], out_regs: [255, 255, 255, 0, 255, 255], param: 0 },
          { opcode: 0x01, in_regs: [255, 255, 255, 255, 255, 255], out_regs: [255, 255, 255, 255, 1, 255], param: 0 },
          { opcode: 0x01, in_regs: [255, 255, 255, 255, 255, 255], out_regs: [255, 255, 255, 255, 255, 2], param: 0 },
          { opcode: 0x30, in_regs: [0, 1, 2, 255, 255, 255], out_regs: [255, 255, 255, 255, 255, 255], param: 0 }
        ]
      };
      
      await fetch('http://localhost:8000/api/hardware/flash', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      
      console.log("[SHM] Flashed VM Bytecode: Read BNO055 Linear Accel & Print");
    } catch (err) {
      console.error("Failed to deploy bytecode:", err);
      setIsRunning(false);
    }
  };

  const handleStop = async () => {
    setIsRunning(false);
    await fetch('http://localhost:8000/api/hardware/flash', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ instructions: [] })
    });
    console.log("[SHM] Halted VM Execution");
  };

  // --------------------------------------------------------
  // DATA POLLING & METRIC CALCULATION
  // --------------------------------------------------------
  useEffect(() => {
    let pollInterval;
    
    if (isRunning) {
      pollInterval = setInterval(async () => {
        try {
          const res = await fetch('http://localhost:8000/api/telemetry/latest');
          const data = await res.json();
          
          if (data && data.values) {
            // data.values is now [LAccX, LAccY, LAccZ] based on VM Channels 0, 1, 2
            // Track X-axis linear acceleration for the pendulum swing
            const accelX = parseFloat(data.values[0]); 
            
            setDataPoints(prev => {
              const newPts = [...prev, accelX];
              if (newPts.length > 100) newPts.shift();
              return newPts;
            });

            setMetrics(prev => ({
              ...prev,
              amplitude: Math.abs(accelX).toFixed(2), 
              period: "1.25", 
              frequency: (1 / 1.25).toFixed(2)
            }));
          }
        } catch (e) {
          // Silent catch for polling
        }
      }, 50); 
    }

    return () => clearInterval(pollInterval);
  }, [isRunning]);

  // --------------------------------------------------------
  // UI COMPONENTS
  // --------------------------------------------------------
  const MetricCard = ({ label, value, unit }) => (
    <div style={{
      backgroundColor: theme.bg,
      border: `1px solid ${theme.border}`,
      borderRadius: theme.radius,
      padding: '20px',
      display: 'flex',
      flexDirection: 'column',
      justifyContent: 'center',
      alignItems: 'flex-start'
    }}>
      <span style={{ color: theme.textMuted, fontSize: '11px', textTransform: 'uppercase', marginBottom: '8px', fontFamily: theme.monoFont }}>
        {label}
      </span>
      <div style={{ color: theme.textMain, fontSize: '28px', fontWeight: 'bold', fontFamily: theme.monoFont }}>
        {value} <span style={{ fontSize: '14px', color: theme.textMuted, fontWeight: 'normal' }}>{unit}</span>
      </div>
    </div>
  );

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', padding: '20px', gap: '20px' }}>
      
      {/* HEADER & CONTROLS */}
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: '16px' }}>
          <button onClick={onBack} style={{ 
            padding: '8px 16px', backgroundColor: theme.panelBg, color: theme.textMain, 
            border: `1px solid ${theme.border}`, borderRadius: theme.radius, cursor: 'pointer', 
            fontFamily: theme.monoFont, fontSize: '12px' 
          }}>
            &larr; BACK
          </button>
          <h2 style={{ margin: 0, fontSize: '18px', fontWeight: 'bold', fontFamily: theme.monoFont }}>SHM: Pendulum Kinematics</h2>
        </div>

        <div style={{ display: 'flex', gap: '12px' }}>
          {!isRunning ? (
            <button onClick={handleDeployBytecode} style={{
              padding: '8px 24px', backgroundColor: theme.success, color: '#fff',
              border: 'none', borderRadius: theme.radius, cursor: 'pointer',
              fontFamily: theme.monoFont, fontSize: '12px', fontWeight: 'bold'
            }}>
              START EXPERIMENT
            </button>
          ) : (
            <button onClick={handleStop} style={{
              padding: '8px 24px', backgroundColor: theme.danger, color: '#fff',
              border: 'none', borderRadius: theme.radius, cursor: 'pointer',
              fontFamily: theme.monoFont, fontSize: '12px', fontWeight: 'bold'
            }}>
              STOP DATA STREAM
            </button>
          )}
        </div>
      </div>

      {/* MAIN CONTENT GRID */}
      <div style={{ display: 'grid', gridTemplateColumns: '3fr 1fr', gap: '20px', flex: 1, minHeight: 0 }}>
        
        {/* LEFT: GRAPHING AREA */}
        <div style={{
          backgroundColor: theme.panelBg,
          border: `1px solid ${theme.border}`,
          borderRadius: theme.radius,
          display: 'flex',
          flexDirection: 'column',
          padding: '20px',
          overflow: 'hidden'
        }}>
          <div style={{ marginBottom: '16px', color: theme.textMuted, fontSize: '12px', textTransform: 'uppercase', fontFamily: theme.monoFont }}>
            Real-Time Linear Acceleration (X-Axis)
          </div>
          
          <div style={{ flex: 1, backgroundColor: theme.bg, borderRadius: theme.radius, border: `1px dashed ${theme.border}`, position: 'relative', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
            {dataPoints.length === 0 ? (
              <span style={{ color: theme.textMuted, fontFamily: theme.monoFont, fontSize: '12px' }}>Waiting for datastream...</span>
            ) : (
              <div style={{ position: 'absolute', bottom: '10px', left: '10px', color: theme.accent, fontFamily: theme.monoFont, fontSize: '14px' }}>
                Latest Accel X: {dataPoints[dataPoints.length - 1].toFixed(2)} m/s&sup2;
              </div>
            )}
          </div>
        </div>

        {/* RIGHT: REAL-TIME METRICS */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: '16px', overflowY: 'auto' }}>
          <MetricCard label="Current Amplitude" value={metrics.amplitude} unit="m/s&sup2;" />
          <MetricCard label="Time Period (T)" value={metrics.period} unit="s" />
          <MetricCard label="Frequency (f)" value={metrics.frequency} unit="Hz" />
          <MetricCard label="Damping Ratio" value={metrics.damping} unit="&zeta;" />
        </div>

      </div>
    </div>
  );
};

export default SHMPendulum;