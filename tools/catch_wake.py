import os, time, sys
import serial
PORT='/dev/cu.usbmodem21201'
t0=time.time()
print("watching for wake window...", flush=True)
while time.time()-t0 < 190:
    if os.path.exists(PORT):
        print(f"[{time.time()-t0:.1f}s] port appeared", flush=True)
        try:
            s=serial.Serial(PORT,115200,timeout=0.2)
            # esptool-style reset into ROM bootloader (USJ translates DTR/RTS)
            s.dtr=False; s.rts=True;  time.sleep(0.06)
            s.dtr=True;  s.rts=False; time.sleep(0.06)
            s.dtr=False
            s.close()
            print("bootloader reset sent", flush=True)
            time.sleep(2.5)
            if os.path.exists(PORT):
                print("PORT STABLE -> download mode. exit 0", flush=True)
                sys.exit(0)
            print("port vanished again; retrying", flush=True)
        except Exception as e:
            print("open failed:", e, flush=True)
            time.sleep(0.2)
    time.sleep(0.05)
print("timeout without catching the window", flush=True)
sys.exit(1)
