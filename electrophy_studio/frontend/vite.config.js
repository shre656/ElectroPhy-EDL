import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [react()],
  build: {
    rollupOptions: {
      output: {
        // This tells Vite to split these heavy libraries into their own separate files
        manualChunks: {
          plotly: ['plotly.js', 'react-plotly.js'],
          reactflow: ['reactflow']
        }
      }
    },
    // Optional: Increases the warning threshold slightly to account for Plotly's natural size
    chunkSizeWarningLimit: 1500 
  }
})