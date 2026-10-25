"""Match mouse-only PC arrivals to accepted phone reports with bounded clock alignment."""
import argparse,bisect,csv,json,re,statistics
from collections import defaultdict,Counter
from pathlib import Path
p=argparse.ArgumentParser(); p.add_argument('--raw',type=Path,required=True)
p.add_argument('--clock',type=Path,required=True); p.add_argument('--process',type=int,required=True)
p.add_argument('--output',type=Path,required=True); p.add_argument('logs',type=Path,nargs='+')
args=p.parse_args(); clock=json.loads(args.clock.read_text()); assert clock['interval_valid']
schema=json.loads(Path(__file__).with_name('control-trace-schema.json').read_text(encoding='utf-8'))
accepted_widths={28,34,34+len(schema['extra_fields'])}
offsets=[]; commands={}
for log in args.logs:
    for line in log.read_text(encoding='utf-8-sig').splitlines():
        try: e=json.loads(line)
        except ValueError: continue
        if e.get('process_id')!=args.process: continue
        if 'monotonic_us' in e: offsets.append(e['timestamp_unix_ms']*1000-e['monotonic_us'])
        m=re.search(r'control_trace_v[456]=\[([^]]*)',e.get('detail',''))
        for row in m.group(1).split(';') if m else []:
            v=row.split(',')
            if len(v) not in accepted_widths: continue
            at,dx,dy=map(int,v[18:21])
            if at: commands[at]=(dx,dy)
assert offsets and commands,'No current process monotonic trace found'
mono_to_pc=statistics.median(offsets)-clock['phone_minus_pc_mid_us']
raw=list(csv.DictReader(args.raw.open()))
raw=[{k:int(v) for k,v in r.items()} for r in raw]
def quantile(values):
    s=sorted(values)
    return {f'p{q}':s[round((len(s)-1)*q/100)] for q in [50,95,99]} if s else {}
devices=[]
for device in sorted(set(r['device_id'] for r in raw)):
    events=[r for r in raw if r['device_id']==device and not r['flags']&1]
    if not events: continue
    start,end=events[0]['unix_us'],events[-1]['unix_us']
    accepted=[(at+mono_to_pc,*xy) for at,xy in sorted(commands.items()) if start-200000<at+mono_to_pc<end]
    by_delta=defaultdict(list)
    for i,r in enumerate(events): by_delta[(r['dx'],r['dy'])].append((r['unix_us'],i))
    histogram=Counter()
    for at,dx,dy in accepted:
        times=by_delta[(dx,dy)]; lo=bisect.bisect_left(times,(at-15000,-1)); hi=bisect.bisect_right(times,(at+150000,10**9))
        for arrival,_ in times[lo:hi]: histogram[round((arrival-at)/2000)*2]+=1
    mode=histogram.most_common(1)[0][0]*1000 if histogram else 0
    used=set(); delays=[]; distinctive=[]
    for at,dx,dy in accepted:
        times=by_delta[(dx,dy)]; lo=bisect.bisect_left(times,(at+mode-18000,-1)); hi=bisect.bisect_right(times,(at+mode+18000,10**9))
        candidates=[(abs(t-at-mode),t,index) for t,index in times[lo:hi] if index not in used]
        if not candidates: continue
        _,t,index=min(candidates); used.add(index); delays.append(t-at)
        if abs(dx)+abs(dy)>=5: distinctive.append(t-at)
    devices.append({'device_id':device,'relative_arrivals':len(events),'duration_ms':(end-start)/1000,
        'arrival_interval_us':quantile([b['unix_us']-a['unix_us'] for a,b in zip(events,events[1:]) if b['unix_us']-a['unix_us']<200000]),
        'accepted_reports_in_interval':len(accepted),'exact_delta_matches':len(delays),'matched_fraction':len(delays)/len(accepted) if accepted else 0,
        'mode_delay_ms':mode/1000,'api_to_raw_arrival_estimate_us':quantile(delays),'distinctive_matches':len(distinctive),
        'distinctive_delay_us':quantile(distinctive),'top_lag_bins_ms':histogram.most_common(6)})
report={'process':args.process,'raw_source':str(args.raw),'clock_half_interval_us':clock['half_interval_us'],
        'logger_epoch_mapping_spread_us':max(offsets)-min(offsets),'devices':devices,
        'semantics':'Mouse arrival at Windows, not rendered game response. Delta matching is correlation, can misassociate repeated small reports. Latency estimates carry clock uncertainty and 1 ms phone logger resolution; device ids are local anonymous ordinals.'}
args.output.write_text(json.dumps(report,indent=2),encoding='utf-8'); print(json.dumps(report,indent=2))
