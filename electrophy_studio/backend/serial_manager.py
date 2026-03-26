import serial
import serial.tools.list_ports
import threading
import time
import os
import csv
from datetime import datetime

import socket

class SerialManager:
    def __init__(self, port=None, baudrate=115200):
        self.port = port
        self.baudrate = baudrate
        self.serial_connection = None
        self.is_running = False
        self.latest_data = None
        
        # Temporary storage mechanisms
        self.experiment_buffer = []
        self.is_recording = False

    @staticmethod
    def auto_detect_pico():
        print("Scanning for ElectroPhy (Raspberry Pi Pico)...")
        ports = serial.tools.list_ports.comports()
        for port in ports:
            # Check for the Vendor ID integer (Windows/Linux) 
            # OR the raw string in the hardware ID (Mac fallback)
            # OR the specific Mac USB modem name
            hwid_str = str(port.hwid).upper()
            device_str = str(port.device).lower()
            
            if (port.vid == 0x2E8A) or ("2E8A" in hwid_str) or ("usbmodem" in device_str):
                print(f"Found device on: {port.device}")
                return port.device
                
        print("No device found.")
        return None

    def connect(self):
        if not self.port:
            self.port = self.auto_detect_pico()
            if not self.port:
                return False

        try:
            self.serial_connection = serial.Serial(self.port, self.baudrate, timeout=1)
            self.is_running = True
            print(f"Connected to {self.port} at {self.baudrate} baud.")
            
            read_thread = threading.Thread(target=self._read_loop, daemon=True)
            read_thread.start()
            return True
            
        except serial.SerialException as e:
            print(f"Failed to connect: {e}")
            return False

    def _read_loop(self):
        while self.is_running and self.serial_connection and self.serial_connection.is_open:
            try:
                if self.serial_connection.in_waiting > 0:
                    line = self.serial_connection.readline().decode('utf-8', errors='ignore').strip()
                    
                    # === ADD THIS MASSIVE DEBUG LINE ===
                    print(f"--> [HARDWARE RX] {line}") 
                    
                    self.latest_data = line
                    
                    if self.is_recording:
                        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                        self.experiment_buffer.append(f"{timestamp},{line}")
                        
            except Exception as e:
                print(f"[SERIAL ERROR] {e}")
                self.is_running = False
                break
    # --- Buffer Management Methods ---

    def start_recording(self):
        """Clears the old buffer and starts recording a new experiment."""
        self.experiment_buffer = []
        self.is_recording = True
        print("Started recording to temporary buffer.")

    def stop_recording(self):
        """Pauses the recording but keeps the data in RAM."""
        self.is_recording = False
        print(f"Stopped recording. {len(self.experiment_buffer)} points in buffer.")

    def clear_buffer(self):
        """Permanently deletes the temporary data."""
        self.experiment_buffer = []
        self.is_recording = False
        print("Temporary buffer cleared.")

    def save_buffer_to_disk(self, filename="experiment.csv"):
        """Writes the RAM buffer to permanent storage on the PC."""
        if not self.experiment_buffer:
            print("No data to save.")
            return False

        # Create a folder for permanent storage if it doesn't exist
        os.makedirs("saved_experiments", exist_ok=True)
        filepath = os.path.join("saved_experiments", filename)

        try:
            with open(filepath, mode='w', newline='') as file:
                writer = csv.writer(file)
                writer.writerow(["PC_Timestamp", "Device_Data"]) # Header
                for row in self.experiment_buffer:
                    writer.writerow(row.split(',', 1))
            print(f"Saved successfully to {filepath}")
            return True
        except Exception as e:
            print(f"Save error: {e}")
            return False

    def send_command(self, payload: bytes):
        if self.serial_connection and self.serial_connection.is_open:
            self.serial_connection.write(payload)

    def disconnect(self):
        self.is_running = False
        if self.serial_connection and self.serial_connection.is_open:
            self.serial_connection.close()
            print("Disconnected.")
            
            
            