import serial, time, sys
PORT='/dev/cu.usbmodem21201'
DUR=float(sys.argv[1]) if len(sys.argv)>1 else 200
t0=time.time()
buf=b''
def stamp(): return f"[{time.time()-t0:7.1f}s]"
reset_done=False
while time.time()-t0 < DUR:
    try:
        s=serial.Serial(PORT, 115200, timeout=1)
        if not reset_done:
            # esptool-style hard reset via RTS
            s.dtr=False; s.rts=True; time.sleep(0.1); s.rts=False
            reset_done=True
            print(stamp(), "== board reset ==", flush=True)
        while time.time()-t0 < DUR:
            line=s.readline()
            if line:
                try: txt=line.decode(errors='replace').rstrip()
                except: txt=repr(line)
                if txt: print(stamp(), txt, flush=True)
    except (serial.SerialException, OSError) as e:
        print(stamp(), f"** port gone/err: {e.__class__.__name__} (deep sleep?) **", flush=True)
        time.sleep(1.0)
print(stamp(), "== capture end ==", flush=True)
