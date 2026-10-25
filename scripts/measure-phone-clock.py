"""Bound the phone/PC wall-clock offset using a persistent ADB shell."""
import argparse, json, queue, subprocess, threading, time
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--device',required=True); p.add_argument('--output',type=Path,required=True)
args=p.parse_args()
shell=subprocess.Popen(['adb','-s',args.device,'shell'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL,text=True,bufsize=1)
lines=queue.Queue()
def read():
    for line in shell.stdout: lines.put((time.time_ns(),line.strip()))
threading.Thread(target=read,daemon=True).start()
samples=[]
try:
    for i in range(12):
        start=time.time_ns(); shell.stdin.write('date +%s%N\n'); shell.stdin.flush()
        while True:
            end,text=lines.get(timeout=5)
            if text.isdigit() and len(text)>=18: break
        phone=int(text)
        if i:
            samples.append({'pc_before_us':start//1000,'pc_after_us':end//1000,'phone_unix_us':phone//1000,
                            'round_trip_us':(end-start)//1000,'offset_min_us':(phone-end)//1000,
                            'offset_max_us':(phone-start)//1000})
finally:
    try: shell.stdin.write('exit\n'); shell.stdin.flush(); shell.wait(timeout=3)
    except (OSError,subprocess.TimeoutExpired): shell.kill()
low=max(s['offset_min_us'] for s in samples); high=min(s['offset_max_us'] for s in samples)
report={'phone_minus_pc_min_us':low,'phone_minus_pc_max_us':high,'interval_valid':low<=high,
        'phone_minus_pc_mid_us':(low+high)/2,'half_interval_us':(high-low)/2,'samples':samples}
args.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k!='samples'},indent=2))
