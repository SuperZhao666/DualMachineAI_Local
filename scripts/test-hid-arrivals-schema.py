"""All supported trace schemas must recover the same known arrival interval."""
import argparse,csv,json,subprocess,sys,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--analyzer',type=Path,default=Path(__file__).with_name('analyze-hid-arrivals.py'));args=p.parse_args()
schema=json.loads(Path(__file__).with_name('control-trace-schema.json').read_text())
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp);clock=root/'clock.json';raw=root/'raw.csv';log=root/'phone.jsonl';out=root/'result.json'
 clock.write_text(json.dumps({'interval_valid':True,'phone_minus_pc_mid_us':0,'half_interval_us':0}))
 with raw.open('w',newline='') as h:
  writer=csv.DictWriter(h,['unix_us','elapsed_us','device_id','dx','dy','flags']);writer.writeheader()
  for i in range(3):writer.writerow(dict(unix_us=1_101_000+i*10_000,elapsed_us=i*10_000,device_id=1,dx=7+i*2,dy=-3,flags=0))
 for version,width in [(4,28),(5,34),(6,34+len(schema['extra_fields']))]:
  rows=[]
  for i in range(3):
   values=[0]*width;values[18:21]=[100_000+i*10_000,7+i*2,-3];rows.append(','.join(map(str,values)))
  log.write_text(json.dumps({'process_id':7,'timestamp_unix_ms':1000,'monotonic_us':1000,'detail':f'control_trace_v{version}=['+';'.join(rows)+']'}))
  run=subprocess.run([sys.executable,str(args.analyzer),'--raw',str(raw),'--clock',str(clock),'--process','7','--output',str(out),str(log)],capture_output=True,text=True)
  assert run.returncode==0,(version,run.stderr)
  device=json.loads(out.read_text())['devices'][0]
  assert device['exact_delta_matches']==3 and device['accepted_reports_in_interval']==3
  assert device['api_to_raw_arrival_estimate_us']=={'p50':2000,'p95':2000,'p99':2000}
  print(f'PASS v{version} trace: three unique reports and known 2ms interval')
