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
        
        self.socket_conn = None
        self.connection_type = None # 'usb' or 'wifi'

        self.is_running = False
        self.is_streaming = False
        self.latest_data = None
        
        self.experiment_buffer = []
        self.is_recording = False

        # Start the Wi-Fi server instantly in the background when Python boots
        self.wifi_thread = threading.Thread(target=self._wifi_server_loop, daemon=True)
        self.wifi_thread.start()

    def _wifi_server_loop(self):
        """Runs forever in the background, waiting for the Pico to connect."""
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        
        try:
            server_socket.bind(('0.0.0.0', 8080))
            server_socket.listen(1)
            print("\n[NETWORK] Background Wi-Fi server listening permanently on Port 8080...")
        except Exception as e:
            print(f"\n[NETWORK FATAL] Could not bind to port 8080: {e}. Is another app using it?")
            return

        while True:
            try:
                # This safely blocks the background thread until the Pico knocks on the door
                conn, addr = server_socket.accept() 
                print(f"\n[NETWORK] SUCCESS! Pico connected wirelessly from {addr}")
                
                # If we were using USB, drop it in favor of the new Wi-Fi connection
                if self.serial_connection and self.serial_connection.is_open:
                    self.serial_connection.close()
                
                self.socket_conn = conn
                self.connection_type = 'wifi'
                self.is_running = True
                
                # Handle the incoming data stream
                self._handle_wifi_client(conn)
                
            except Exception as e:
                print(f"[NETWORK] Server error: {e}")
                time.sleep(1)

    def _handle_wifi_client(self, conn):
        """Reads data from the Pico until the connection drops."""
        buffer = ""
        while self.is_running and self.connection_type == 'wifi':
            try:
                data = conn.recv(1024).decode('utf-8', errors='ignore')
                if not data:
                    print("\n[NETWORK] Connection closed by Pico. Listening for reconnect...")
                    self.socket_conn = None
                    self.is_running = False
                    break
                
                buffer += data
                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1)
                    self._process_line(line.strip())
            except Exception as e:
                print(f"\n[NETWORK] Wi-Fi stream lost: {e}. Listening for reconnect...")
                self.socket_conn = None
                self.is_running = False
                break

    @staticmethod
    def auto_detect_pico():
        ports = serial.tools.list_ports.comports()
        for port in ports:
            hwid_str = str(port.hwid).upper()
            device_str = str(port.device).lower()
            if (port.vid == 0x2E8A) or ("2E8A" in hwid_str) or ("usbmodem" in device_str):
                return port.device
        return None

    def connect(self):
        """Now used strictly as a manual fallback for USB connections."""
        if self.connection_type == 'wifi' and self.is_running:
            print("[NETWORK] Already connected via Wi-Fi. Ignoring USB request.")
            return True

        print("\n[USB] Scanning for ElectroPhy (Raspberry Pi Pico) over USB...")
        if not self.port:
            self.port = self.auto_detect_pico()
            if not self.port:
                print("[ERROR] No USB device found.")
                return False

        try:
            self.serial_connection = serial.Serial(self.port, self.baudrate, timeout=1)
            self.is_running = True
            self.connection_type = 'usb'
            print(f"[USB] Connected to {self.port} at {self.baudrate} baud.")
            
            read_thread = threading.Thread(target=self._usb_read_loop, daemon=True)
            read_thread.start()
            return True
            
        except serial.SerialException as e:
            print(f"[USB ERROR] Failed to connect: {e}")
            return False

    def _usb_read_loop(self):
        while self.is_running and self.connection_type == 'usb' and self.serial_connection and self.serial_connection.is_open:
            try:
                if self.serial_connection.in_waiting > 0:
                    line = self.serial_connection.readline().decode('utf-8', errors='ignore').strip()
                    self._process_line(line)
            except Exception as e:
                print(f"\n[USB ERROR] Connection lost: {e}")
                self.is_running = False
                break

    def _process_line(self, line):
        if self.is_streaming:
            # Uncomment below to see raw data in terminal (using \r so it doesn't scroll infinitely)
            # print(f"\r--> [HARDWARE RX] {line:<30}", end="", flush=True) 
            self.latest_data = line
            if self.is_recording:
                timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                self.experiment_buffer.append(f"{timestamp},{line}")

    def send_command(self, payload: bytes):
        if self.connection_type == 'wifi' and self.socket_conn:
            try:
                self.socket_conn.sendall(payload)
            except Exception as e:
                print(f"[TX ERROR] Wi-Fi send failed: {e}")
        elif self.connection_type == 'usb' and self.serial_connection and self.serial_connection.is_open:
            self.serial_connection.write(payload)

    # --- Buffer Management Methods (Unchanged) ---
    def start_recording(self):
        self.experiment_buffer = []
        self.is_recording = True

    def stop_recording(self):
        self.is_recording = False

    def clear_buffer(self):
        self.experiment_buffer = []
        self.is_recording = False

    def save_buffer_to_disk(self, filename="experiment.csv"):
        if not self.experiment_buffer: return False
        os.makedirs("saved_experiments", exist_ok=True)
        filepath = os.path.join("saved_experiments", filename)
        try:
            with open(filepath, mode='w', newline='') as file:
                writer = csv.writer(file)
                writer.writerow(["PC_Timestamp", "Device_Data"]) 
                for row in self.experiment_buffer:
                    writer.writerow(row.split(',', 1))
            return True
        except Exception:
            return False

    def disconnect(self):
        self.is_running = False
        if self.connection_type == 'wifi' and self.socket_conn:
            self.socket_conn.close()
            print("[NETWORK] Wi-Fi Disconnected.")
        elif self.connection_type == 'usb' and self.serial_connection:
            self.serial_connection.close()
            print("[USB] Disconnected.")