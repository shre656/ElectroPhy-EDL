# ElectroPhy Studio

A full-stack diagnostic and visual programming pipeline for the Raspberry Pi Pico. This project consists of custom C firmware for the Pico, a Python FastAPI backend for real-time serial communication, and a React/Vite frontend utilizing React Flow for node-based hardware control and data plotting.

##  Cloning the Repository

**CRITICAL:** Because this project uses the Raspberry Pi Pico SDK as a submodule, you *must* use the `--recurse-submodules` flag when downloading it to ensure the SDK is included.

`git clone --recurse-submodules https://github.com/shre656/ElectroPhy-EDL.git`
`cd ElectroPhy-EDL`

*(If you already cloned it normally, you can fetch the SDK later by running `git submodule update --init --recursive` inside the repo).*

##  Project Structure

* `pico_code/`: C/C++ firmware utilizing the Pico SDK. Handles I2C sensor reading (MPU6050) and a custom bytecode virtual machine.
* `electrophy_studio/backend/`: FastAPI Python server handling serial routing, websocket data streams, and compiling visual graphs into Pico bytecode.
* `electrophy_studio/frontend/`: React/Vite frontend for the visual node-based editor and real-time data plotting.

##  Prerequisites

Ensure you have the following installed on your system:
* **C/C++ Build Tools:** CMake and the ARM GCC Toolchain (for compiling Pico firmware)
* **Python:** Version 3.10+
* **Node.js:** Version 18+ and npm

---

##  Installation & Setup

### 1. Hardware Firmware (Raspberry Pi Pico W)
1. Navigate to the Pico code directory:
   `cd pico_code`
2. Create a build directory and compile the firmware:
   `mkdir build_w && cd build_w`
   `cmake ..`
   `make`
3. Hold the **BOOTSEL** button on your Pico and plug it into your computer via USB.
4. Drag and drop the generated `.uf2` file onto the mounted `RPI-RP2` volume.

### 2. Backend Server (FastAPI)
1. Open a new terminal and navigate to the backend directory:
   `cd electrophy_studio/backend`
2. Create and activate a Python virtual environment:
   `python -m venv backend_venv`
   `source backend_venv/bin/activate`  *(On Windows use: `backend_venv\Scripts\activate`)*
3. Install the required Python packages:
   `pip install -r requirements.txt`
4. Start the backend server:
   `uvicorn main:app --reload`

### 3. Frontend Web App (React)
1. Open a new terminal and navigate to the frontend directory:
   `cd electrophy_studio/frontend`
2. Install the Node dependencies:
   `npm install`
3. Start the Vite development server:
   `npm run dev`

---

##  Usage

1. Ensure your Pico is connected via a data-capable USB cable and wired to the sensor (e.g., MPU6050).
2. Verify the backend FastAPI server is running (`http://localhost:8000`).
3. Open the frontend URL provided by Vite (usually `http://localhost:5173`) in your web browser.
4. Build your data pipeline using the node editor, then click **Compile & Deploy** to push the bytecode to the Pico.
5. Watch the live data stream in the plot window!
