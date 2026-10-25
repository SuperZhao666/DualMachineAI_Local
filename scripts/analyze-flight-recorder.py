"""Join observation -> native ticket -> terminal delivery, with explicit clock domains.

This is a diagnostic correlation, not player-intent, hit-rate or physical-motion truth.
"""
import argparse, csv, json, math, re, statistics
from collections import Counter
from pathlib import Path
from control_trace_flags import ControlFlagInterpreter, LAYOUT_CHOICES

p=argparse.ArgumentParser()
p.add_argument('collection',type=Path); p.add_argument('--process',type=int,required=True)
p.add_argument('--control-csv',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--replay-output',type=Path,help='Optional local numeric detection replay; excludes images and raw input keys')
p.add_argument('--windows-output',type=Path,help='Bounded diagnostic windows around reversals, position jumps and switches; not ground-truth faults')
p.add_argument('--control-flag-layout',choices=LAYOUT_CHOICES,default='auto',
               help='Explicit interpretation override; auto requires this process/trace manifest.')
a=p.parse_args()
schema=json.loads(Path(__file__).with_name('control-trace-schema.json').read_text(encoding='utf-8'))
flag_interpreter=ControlFlagInterpreter(schema,a.process,a.control_flag_layout)
dispatch={};delivery={};detections={};anchors=[];costs=[];losses=Counter(); errors=[]; duplicates=Counter()
# Require a complete numeric token: app_version=1.3.1 is metadata, not a
# float. Version manifests are also the evidence for the flag layout below.
number=re.compile(r'\b([a-z_]+)=(-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)(?=$|[\s,;}\]])')
events=[]
for path in a.collection.glob('visionforge-mobile-runtime.jsonl*'):
 for line in path.open(encoding='utf-8-sig'):
  try:e=json.loads(line)
  except ValueError:continue
  if e.get('process_id')==a.process:events.append(e)
events.sort(key=lambda e:e['sequence'])
for e in events:
 flag_interpreter.consume(e)
 detail=e.get('detail','');nums={k:float(v) for k,v in number.findall(detail)}
 if e['event']=='runtime_clock_anchor':
  anchors.append(nums)
 if e['event']=='runtime_trace_cost':costs.append(nums)
 if e['event']=='runtime_trace_format_error':errors.append(detail)
 if e['event']=='runtime_trace_batch':
  for key in ['control_trace_lost','dispatch_trace_lost','delivery_trace_lost','detection_trace_lost']:losses[key]+=int(nums.get(key,0))
 m=re.search(r'detection_trace_v1=\[([^]]*)',detail)
 if m:
  for encoded in filter(None,m[1].split(';')):
   vals=encoded.split(',')
   try:
    kept=int(vals[11]);assert 0<=kept<=16 and len(vals)==12+6*kept
    row=dict(zip(schema['detection_header_fields'],map(float,vals[:12])))
    row['boxes']=[dict(zip(schema['detection_box_fields'],map(float,vals[12+n*6:18+n*6]))) for n in range(kept)]
    row['trace_id']=e['trace_id'];key=(e['trace_id'],int(row['id']))
    if key in detections:duplicates['detection']+=1
    detections[key]=row
   except (ValueError,IndexError,AssertionError):errors.append('detection:field_count_or_values')
 for name,store in [('dispatch',dispatch),('delivery',delivery)]:
  m=re.search(name+r'_trace_v1=\[([^]]*)',detail)
  if not m:continue
  for encoded in filter(None,m[1].split(';')):
   vals=encoded.split(',');fields=schema[name+'_fields']
   if len(vals)!=len(fields):errors.append(name+':field_count');continue
   row=dict(zip(fields,map(int,vals)));row['trace_id']=e['trace_id']
   row['route']='bluetooth_hid' if 'route=bluetooth_hid' in detail else 'makcu_usb'
   key=(e['trace_id'],row['id'],row['route'])
   if key in store:duplicates[name]+=1
   store[key]=row
with a.control_csv.open(encoding='utf-8-sig',newline='') as h:
 control=[dict(row) for row in csv.DictReader(h)]
for row in control:
 for k,v in list(row.items()):
  if k!='trace_id':row[k]=float(v)
controls={(r['trace_id'],int(r['id'])):r for r in control}
raw_frames={(r['trace_id'],r['sequence'],r['wire_frame_id'],r['observed_us']):r for r in detections.values()}
control_raw_matches=sum((r['trace_id'],r['sequence'],r['wire_frame_id'],r['observed_us']) in raw_frames for r in control)
deliveries={}
for row in delivery.values():deliveries.setdefault((row['trace_id'],row['ticket']),[]).append(row)

def q(values):
 values=sorted(v for v in values if math.isfinite(v))
 return {'n':len(values),**{f'p{x}':values[round((len(values)-1)*x/100)] for x in (50,95,99)},'max':max(values)} if values else {'n':0}

def monotonic_time(row, key):
 value=row[key]
 if not value:return None
 if row['route']=='bluetooth_hid':return value
 if not anchors:return None
 anchor=min(anchors,key=lambda v:abs(v['elapsed_realtime_us']-value))
 return value-anchor['elapsed_realtime_us']+(anchor['monotonic_before_us']+anchor['monotonic_after_us'])/2

queue=[];write=[];echo=[];reader_wake=[];observation_to_echo=[];joins=[];unmatched=Counter();unsubmitted=Counter()
for d in dispatch.values():
 c=controls.get((d['trace_id'],d['control_trace_id']))
 matches=deliveries.get((d['trace_id'],d['ticket']),[]) if d['ticket'] else []
 if len(matches)!=1:
  if d['outcome']!=1:
   unsubmitted[str(d['outcome'])]+=1;continue
  unmatched['no_unique_terminal_record']+=1;continue
 t=matches[0]
 if c is None:unmatched['control_frame_not_retained']+=1;continue
 if (int(c['sequence'])!=d['sequence'] or int(c['dx'])!=d['dx'] or int(c['dy'])!=d['dy']):
  unmatched['source_frame_mismatch']+=1;continue
 if (t['dx'],t['dy'])!=(d['dx'],d['dy']):unmatched['delivery_delta_mismatch']+=1;continue
 if t['started_us'] and t['offered_us']:
  queue.append(t['started_us']-t['offered_us'])
 if t['returned_us'] and t['started_us']:write.append(t['returned_us']-t['started_us'])
 if t['acknowledged_us'] and t['offered_us']:
  echo.append(t['acknowledged_us']-t['offered_us'])
  reader_wake.append(t['completed_us']-t['acknowledged_us'])
  mono=monotonic_time(t,'acknowledged_us')
  if mono is not None:observation_to_echo.append(mono-c['observed_us'])
 joins.append({'ticket':d['ticket'],'control_id':d['control_trace_id'],'lock_id':int(c['lock_id']),
   'dx':d['dx'],'dy':d['dy'],'outcome':t['outcome'],'total_us':t['total_us'],
   'age_us':c['processed_us']-c['observed_us'],'prediction_horizon_us':c.get('servo_horizon_seconds',0)*1e6,
   'velocity_x':c['velocity_x'],'velocity_y':c['velocity_y'],'raw_error_x':c['target_x']-c.get('coordinate_width',c.get('model_width',416))/2,
   'raw_error_y':c['target_y']-c.get('coordinate_height',c.get('model_height',416))/2,'predicted_error_x':c['predicted_error_x'],'predicted_error_y':c['predicted_error_y']})
active=[r for r in control if int(r['flags'])&1 and not int(r['flags'])&(2|16)]
flag_summary=flag_interpreter.summarize(control)
consecutive=[(x,y) for x,y in zip(control,control[1:]) if x['trace_id']==y['trace_id']
 and y['id']==x['id']+1 and x['lock_id']==y['lock_id'] and x['track_id']==y['track_id']
 and int(x['flags'])&1 and int(y['flags'])&1 and not (int(x['flags'])|int(y['flags']))&(2|16)
 and 0<y['observed_us']-x['observed_us']<=50000]
reverse={axis:sum(x['d'+axis]*y['d'+axis]<0 and min(abs(x['d'+axis]),abs(y['d'+axis]))>=2 for x,y in consecutive) for axis in 'xy'}
window_reasons={}
for x,y in consecutive:
 reasons=[]
 for axis in 'xy':
  if x['d'+axis]*y['d'+axis]<0 and min(abs(x['d'+axis]),abs(y['d'+axis]))>=2:reasons.append('output_reversal_'+axis)
 if math.hypot(y['target_x']-x['target_x'],y['target_y']-x['target_y'])>20:reasons.append('same_identity_position_step_above_20px')
 if reasons:window_reasons[(y['trace_id'],y['id'])]=reasons
for r in control:
 if int(r['flags'])&4:window_reasons.setdefault((r['trace_id'],r['id']),[]).append('identity_switch')
 if int(r.get('velocity_flags',0))&192:window_reasons.setdefault((r['trace_id'],r['id']),[]).append('stationary_anchor_geometry_guard')
windows=[]
window_event_count=len(window_reasons)
# A guard can remain active for many consecutive frames. Reserve at most 16
# windows for those diagnostics so they cannot evict all jumps and switches.
geometry_only={key for key,reasons in window_reasons.items()
 if reasons==['stationary_anchor_geometry_guard']}
selected=sorted(set(window_reasons)-geometry_only)[-64:]
selected+=sorted(geometry_only)[-min(16,64-len(selected)):] if len(selected)<64 else []
export_keys=set(selected) if a.windows_output else set()
for n,r in enumerate(control):
 if (r['trace_id'],r['id']) not in export_keys:continue
 reasons=window_reasons.get((r['trace_id'],r['id']))
 if not reasons:continue
 frames=[]
 for sample in control[max(0,n-6):n+7]:
  if sample['trace_id']!=r['trace_id']:continue
  raw=raw_frames.get((sample['trace_id'],sample['sequence'],sample['wire_frame_id'],sample['observed_us']))
  frames.append({'control':sample,'detection_input':raw})
 windows.append({'control_id':int(r['id']),'reasons':reasons,'frames':frames})
report={'process':a.process,'control_frames':len(control),'schema_counts':dict(Counter(int(r.get('schema_version',0)) for r in control)),
 'detection_frames':len(detections),'frames_with_detections':sum(r['total_detections']>0 for r in detections.values()),
 'frames_with_multiple_detections':sum(r['total_detections']>1 for r in detections.values()),
 'detection_clipped_frames':sum(r['total_detections']>r['kept_detections'] for r in detections.values()),
 'control_frames_with_raw_detection_input':control_raw_matches,
 'dispatch_records':len(dispatch),'delivery_records':len(delivery),'joined_commands':len(joins),'join_gaps':dict(unmatched),
 'accepted_dispatches':sum(d['outcome']==1 for d in dispatch.values()),'unsubmitted_native_outcomes':dict(unsubmitted),
 'trace_overruns':dict(losses),'duplicate_records':dict(duplicates),'format_errors':errors,
 'terminal_outcomes':dict(Counter(r['outcome'] for r in delivery.values())),
 'native_offer_outcomes':dict(Counter(r['outcome'] for r in dispatch.values())),
 'queue_us':q(queue),'usb_write_call_us':q(write),'offer_to_firmware_echo_us':q(echo),
 'echo_to_writer_resume_us':q(reader_wake),'observation_to_firmware_echo_us':q(observation_to_echo),
 'clock_anchor_count':len(anchors),'clock_anchor_bracket_us':q([v['monotonic_after_us']-v['monotonic_before_us'] for v in anchors]),
 'active_observation_age_us':q([r['processed_us']-r['observed_us'] for r in active]),
 'actionable_head_sources': flag_summary['actionable_head_sources'],
 'large_background_motion_recovered_frames':flag_summary['large_background_motion_recovered_frames'],
 'active_large_background_motion_recovered_frames':flag_summary['active_large_background_motion_recovered_frames'],
 'configured_geometric_head_frames':flag_summary['configured_geometric_head_frames'],
 'control_flag_semantics':flag_summary,
 'active_prediction_horizon_us':q([r['servo_horizon_seconds']*1e6 for r in active if int(r.get('motion_flags',0))&1]),
 'active_prediction_lead_pixels':q([math.hypot(r['lead_x'],r['lead_y']) for r in active if int(r.get('motion_flags',0))&1]),
 'active_prediction_limited_frames':sum(bool(int(r.get('motion_flags',0))&2) for r in active),
 'active_output_limited_frames':sum(bool(int(r.get('motion_flags',0))&4) for r in active),
 'stationary_anchor_geometry_guard_frames':{axis:sum(bool(int(r.get('velocity_flags',0))&bit) for r in active)
   for axis,bit in [('x',64),('y',128)]},
 'velocity_estimator_disagreement_pixels_per_second':q([math.hypot(r['proposal_x']-r['disturbance_x'],r['proposal_y']-r['disturbance_y'])
   for r in active if int(r.get('velocity_flags',0))&15==15]),
 'diagnostic_snapshot_format_us':q([r['native_snapshot_format_us'] for r in costs]),
 'diagnostic_snapshot_enqueue_us':q([r['total_snapshot_enqueue_us'] for r in costs]),
 'diagnostic_characters_per_batch':q([r['characters'] for r in costs]),
 'same_identity_consecutive_pairs':len(consecutive),'same_identity_output_direction_reversals_at_least_2_counts':reverse,
 'crowd_active_frames':sum(r.get('candidate_count',0)>1 for r in active),
 'pending_switch_frames':sum(bool(int(r.get('selection_flags',0))&4) for r in control),
 'diagnostic_window_events':window_event_count,'diagnostic_windows_exported':len(windows),
 'slowest_joined_commands':sorted(joins,key=lambda r:r['total_us'],reverse=True)[:12],
 'limitations':['Firmware echo does not measure physical execution or game response.',
  'Observation timestamp excludes unknown one-way network latency.',
  'Player mouse/WSAD intention is unobserved; reversals may be correct, not jitter labels.',
  'Unmatched records may be at a batch/window boundary or rejected before enqueue; do not call them dropped movement.',
  'Command timings join one process by native ticket and source control-trace id; do not combine process restarts.']}
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
if a.replay_output:
 a.replay_output.parent.mkdir(parents=True,exist_ok=True)
 a.replay_output.write_text('\n'.join(json.dumps(detections[k],separators=(',',':')) for k in sorted(detections)),encoding='utf-8')
if a.windows_output:
 a.windows_output.parent.mkdir(parents=True,exist_ok=True)
 a.windows_output.write_text(json.dumps({'semantics':'Candidate investigation windows, not verified faults or user-input labels.',
   'control_flag_semantics':flag_summary,'total_events':window_event_count,'latest_bounded_windows':windows},indent=2),encoding='utf-8')
print(json.dumps({k:report[k] for k in ['control_frames','dispatch_records','delivery_records','joined_commands','join_gaps','format_errors','queue_us','offer_to_firmware_echo_us','diagnostic_snapshot_enqueue_us']},indent=2))
