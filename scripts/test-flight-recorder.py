"""Exercise cross-clock joins, v5 compatibility, and partial-window semantics."""
from pathlib import Path
import csv, json, subprocess, sys, tempfile

root=Path(__file__).resolve().parents[1]
schema=json.loads((root/'scripts/control-trace-schema.json').read_text(encoding='utf-8'))
with tempfile.TemporaryDirectory(prefix='vf-flight-') as temporary:
 d=Path(temporary);events=[]
 def event(name,detail):
  events.append(dict(process_id=7,trace_id='test-process',sequence=len(events)+1,
    timestamp_utc='2026-10-19T00:00:00Z',monotonic_us=140000,event=name,detail=detail))
 event('runtime_session_manifest','app_version=1.3.1-measured-head-continuity version_code=39')
 control=[1,42,100000,130000,1,3,230,208,4,0,0,32769,0,0,0,0,0,0,0,0,0,100,0,24,0,1,1,40000,77,210,180,250,260,.9]
 extra=[0]*len(schema['extra_fields']);extra[schema['extra_fields'].index('motion_flags')]=1
 extra[schema['extra_fields'].index('servo_horizon_seconds')]=.07
 extra[schema['extra_fields'].index('velocity_flags')]=128
 event('runtime_control_trace_v6','control_trace_v6=['+','.join(map(str,control+extra))+']')
 old=control.copy();old[0]=2;old[1]=43;old[11]=0
 event('runtime_control_trace','control_trace_v5=['+','.join(map(str,old))+']')
 event('runtime_dispatch_trace_v1','dispatch_trace_v1=[1,42,99,130100,130200,20000,4,0,1,1;2,43,100,140000,140100,20000,4,0,5,2]')
 event('runtime_delivery_trace_v1','route=makcu_usb delivery_trace_v1=[1,99,4,0,1130110,1130210,1130310,1130710,1130610,1150110,1,1,16,600]')
 event('runtime_detection_trace_v1','detection_trace_v1=[1,42,77,100000,130000,90000,1,2.5,-1,16,1,1,0,210,180,250,260,0.9]')
 event('runtime_clock_anchor','monotonic_before_us=130000 elapsed_realtime_us=1130000 unix_ms=1000000 monotonic_after_us=130000')
 (d/'visionforge-mobile-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events),encoding='utf-8')
 trace=d/'control.csv';report=d/'report.json'
 subprocess.run([sys.executable,str(root/'scripts/extract-control-trace.py'),str(d/'visionforge-mobile-runtime.jsonl'),
    '--process','7','--output',str(trace)],check=True,stdout=subprocess.DEVNULL)
 subprocess.run([sys.executable,str(root/'scripts/analyze-flight-recorder.py'),str(d),'--process','7',
    '--control-csv',str(trace),'--output',str(report)],check=True,stdout=subprocess.DEVNULL)
 r=json.loads(report.read_text(encoding='utf-8'))
 assert r['control_frames']==2 and r['schema_counts']=={'6':1,'5':1}
 assert r['joined_commands']==1 and r['join_gaps']=={} and r['unsubmitted_native_outcomes']=={'5':1}
 assert r['detection_frames']==1 and r['frames_with_detections']==1 and r['detection_clipped_frames']==0
 assert r['queue_us']['p50']==100 and r['offer_to_firmware_echo_us']['p50']==500
 assert r['observation_to_firmware_echo_us']['p50']==30610
 assert r['echo_to_writer_resume_us']['p50']==100
 assert r['stationary_anchor_geometry_guard_frames']=={'x':0,'y':1}
 assert r['actionable_head_sources']=={'measured':1,'projected':0}
 assert not r['format_errors']
 print('PASS flight recorder v6/v5, ticket/source joins, elapsed-to-monotonic mapping, partial-window gap semantics')
