"""Index bounded diagnostic logs and summarize each clock domain independently.

The SQLite index retains numeric control/dispatch/delivery/detection rows for
subsequent replay, while the JSON report flags missing data and excludes claims
about physical HID execution, player intent, or manually labeled accuracy.
"""
from pathlib import Path
import argparse,collections,hashlib,json,math,re,sqlite3
from decimal import Decimal,localcontext

FIXED={'pipeline_trace_v1':'pipeline_fields','pipeline_outcome_v1':'pipeline_outcome_fields',
       'host_frame_trace_v1':'host_frame_fields','dispatch_trace_v1':'dispatch_fields',
       'host_frame_trace_v2':'host_frame_v2_fields',
       'host_frame_trace_v3':'host_frame_v3_fields',
       'image_motion_trace_v1':'image_motion_fields','visible_response_trace_v1':'visible_response_fields',
       'projection_trace_v1':'projection_fields','projection_target_trace_v1':'projection_target_fields',
       'disturbance_selection_trace_v1':'disturbance_selection_fields',
       'delivery_trace_v1':'delivery_fields'}
ARRAY=re.compile(r'\b([a-z_]+_v\d+)=\[([^]]*)\]')
NUMBER=re.compile(r'\b([a-z_]+)=(-?\d+(?:\.\d+)?)\b')
BASE_CONTROL=['id','sequence','observed_us','processed_us','lock_id','track_id','target_x','target_y',
 'dx','dy','reason','flags','background_x','background_y','background_support','pending_x','pending_y',
 'pending_reports','accepted_at_us','accepted_dx','accepted_dy','velocity_x','velocity_y','predicted_error_x',
 'predicted_error_y','response_x','response_y','response_delay_us','wire_frame_id','box_x1','box_y1','box_x2','box_y2','confidence']

def quantiles(values):
    values=sorted(values)
    return {'n':len(values),**{f'p{q}':values[round((len(values)-1)*q/100)] for q in (50,95,99)}} if values else {'n':0}

def observation_key(d):
    return (d['sequence'],d['wire_frame_id'],d['observed_us'])

def projection_key(d):
    return tuple(d[k] for k in ('generation','mode','a_sequence','a_wire_frame_id','a_observed_us',
                                'b_sequence','b_wire_frame_id','b_observed_us'))

def core_timing(contexts,generations):
    """Actual valid Core contexts; no NN FPS or physical movement inference."""
    rows=sorted(contexts.values(),key=lambda d:(d['processed_us'],d['observed_us'],d['sequence']))
    gaps=[];pixel_gaps=[];segments=[];previous=None
    for d in rows:
        generation=generations.get(observation_key(d))
        restart=previous is None or d['observed_us']<=previous['observed_us'] or d['sequence']<=previous['sequence']
        if previous is not None:
            before=generations.get(observation_key(previous))
            restart|=generation is not None and before is not None and generation!=before
        if restart:
            segments.append({'generation':generation,'observations':1,'first_processed_us':d['processed_us'],
                'last_processed_us':d['processed_us'],'first_observed_us':d['observed_us'],'last_observed_us':d['observed_us']})
        else:
            gaps.append(d['processed_us']-previous['processed_us']);pixel_gaps.append(d['observed_us']-previous['observed_us'])
            segment=segments[-1];segment['observations']+=1;segment['last_processed_us']=d['processed_us'];segment['last_observed_us']=d['observed_us']
        previous=d
    seconds=sum((s['last_processed_us']-s['first_processed_us'])/1e6 for s in segments)
    intervals=sum(max(0,s['observations']-1) for s in segments)
    full_rates=[]
    for s in segments:
        first=(s['first_processed_us']//1_000_000+1)*1_000_000
        last=s['last_processed_us']//1_000_000*1_000_000
        bins=collections.Counter(d['processed_us']//1_000_000*1_000_000 for d in rows
            if s['first_processed_us']<=d['processed_us']<=s['last_processed_us'])
        full_rates.extend(bins.get(at,0) for at in range(first,last,1_000_000))
    return {'observations':len(rows),'segments':segments,'measured_interval_seconds':seconds,
        'effective_processing_hz':intervals/seconds if seconds>0 else None,
        'processed_gap_us':quantiles(gaps),'pixel_observation_gap_us':quantiles(pixel_gaps),
        'processed_gap_max_us':max(gaps,default=None),'processed_gaps_over50ms':sum(v>50000 for v in gaps),
        'processed_gaps_over100ms':sum(v>100000 for v in gaps),'full_one_second_processing_hz':quantiles(full_rates),
        'semantics':'Accepted Core observations include empty/held/tracking-only states; Hz is not movement or physical execution. Segments split only for known generation changes or nonmonotonic context resets.'}

def active_projection_contract(d):
    return (d['mode']==2 and d['generation']>0 and d['a_sequence']>0 and d['a_wire_frame_id']>0 and
        d['b_sequence']>d['a_sequence'] and d['b_wire_frame_id']>0 and
        d['current_nnp_sequence']==d['a_sequence'] and d['current_nnp_wire_frame_id']==d['a_wire_frame_id'] and
        d['current_nnp_observed_us']==d['a_observed_us'] and
        d['control_sequence']==d['b_sequence'] and d['control_wire_frame_id']==d['b_wire_frame_id'] and
        d['control_observed_us']==d['b_observed_us']>d['a_observed_us']>0 and
        d['control_observed_us']>d['previous_control_observed_us'] and
        0<d['source_count']<=16 and d['valid_count']==d['source_count'] and d['gate_reason']==d['math_reason']==0 and
        d['math_work_us']>0 and d['a_observed_us']<=d['started_us']<=d['finished_us'] and
        d['b_observed_us']<=d['finished_us']<d['a_deadline_us'] and
        0<d['a_deadline_us']-d['a_observed_us']<=50000)


def comparison_contract(d):
    return (d['event']==2 and d['outcome']==13 and 0<d['source_count']<=16 and
        d['current_nnp_sequence']==d['b_sequence'] and
        d['current_nnp_wire_frame_id']==d['b_wire_frame_id'] and
        d['current_nnp_observed_us']==d['b_observed_us'] and
        0<=d['comparison_matched']<=min(d['source_count'],d['nnp_count']) and
        0<=d['minimum_iou_e6']<=1_000_000 and
        ((d['comparison_matched']>0 and d['mean_18point_error_milli']>=0) or
         (d['comparison_matched']==0 and d['mean_18point_error_milli']==-1)))

def disturbance_selection_contract(d):
    """Validate integer evidence without treating selected velocity as HID I/O."""
    if not (all(type(v) is int for v in d.values()) and
        d['id']>0 and d['control_trace_id']>0 and d['sequence']>0 and
        d['processed_us']>=d['observed_us']>0 and d['axis'] in (0,1) and
        d['wire_frame_id']>=0 and d['lock_id']>=0 and d['track_id']>=0 and
        0<d['event_flags']<4096 and d['event_flags']&63 and d['branch'] in (0,1,2,3) and
        d['window'] in (0,1,2) and 0<=d['point_count']<=6 and
        0<=d['available_commands']<=192 and 0<=d['quality_weight_e6']<=1_000_000 and
        d['raw_quality_e6']>=0 and d['current_delay_us']>=0 and d['restored_delay_us']>=0 and
        d['frame_interval_us']>=0 and 0<=d['retired_axes']<=3 and 0<=d['geometry_axes']<=3):return False
    flags=d['event_flags'];interval=bool(flags&128);valid=bool(flags&256)
    if d['point_count']:
        if not (0<d['first_point_us']<=d['last_point_us']==d['observed_us'] and
                d['span_us']==d['last_point_us']-d['first_point_us']):return False
    elif any(d[k] for k in ('first_point_us','last_point_us','span_us')):return False
    if interval:
        if not (d['point_count']>=2 and d['minimum_velocity_milli']<=d['maximum_velocity_milli'] and
                0<=d['delay_low_us']<=d['delay_high_us'] and
                d['delay_hypotheses']==(d['delay_high_us']-d['delay_low_us'])//4000+1):return False
    elif any(d[k] for k in ('minimum_velocity_milli','maximum_velocity_milli','raw_quality_e6',
                           'quality_weight_e6','delay_low_us','delay_high_us','delay_hypotheses')):return False
    if d['branch']==0 and valid:return False
    if d['branch']==1 and not (interval and valid and d['minimum_velocity_milli']>=-20000 and
                               d['maximum_velocity_milli']<=20000):return False
    if d['branch'] in (2,3) and not (interval and d['point_count']>=4):return False
    if d['branch']==2 and not valid:return False
    if d['window']==1 and d['point_count']!=4:return False
    if d['window']==2 and d['point_count']<4:return False
    if flags&2048 and abs(d['target_error_milli'])>16001:return False
    # Milli rounding can lose at most one unit around the strict >100px/s edge.
    if flags&8 and not (flags&2048 and abs(d['selected_velocity_milli']-d['previous_selected_milli'])>=99999):return False
    return True


def legacy_six_significant_matches(scaled_integer,reported,scale,offset=0):
    """Compare a precise scaled field to defaultfloat precision6 evidence.

    Bound = half of the reported value's last significant decimal place plus
    half a new-field quantization unit. Offset is applied AFTER determining
    the legacy magnitude: target_error was derived from a rounded target
    coordinate, whose rounding error is larger than its near-zero error's.
    Decimal arithmetic keeps the boundary and integer quantization exact.
    """
    if type(scaled_integer) is not int or scale<=0 or not math.isfinite(reported) or not math.isfinite(offset):return False
    with localcontext() as context:
        context.prec=80
        legacy=Decimal(str(reported));unit=Decimal(scale)
        legacy_half=Decimal(0) if legacy.is_zero() else Decimal('0.5')*Decimal(10)**(legacy.adjusted()-5)
        quantization_half=Decimal('0.5')/unit
        difference=abs(Decimal(scaled_integer)/unit-(legacy-Decimal(str(offset))))
        return difference<=legacy_half+quantization_half


def disturbance_selection_summary(stored,schema,metrics,bad,lost,reset_reports):
    """Event-conditioned evidence, joined to exact Core context, never execution."""
    by_id=collections.defaultdict(list);rows=[]
    for kind,row,width,height in stored:
        if kind=='control_trace_v6':
            d=dict(zip(BASE_CONTROL+schema['extra_fields'],row))
            d['_coordinate_width']=width;d['_coordinate_height']=height;by_id[d['id']].append(d)
        elif kind=='disturbance_selection_trace_v1':rows.append(dict(zip(schema['disturbance_selection_fields'],row)))
    rows.sort(key=lambda d:(d['processed_us'],d['observed_us'],d['id'],d['axis']))
    counts=collections.Counter({key:0 for key in ('valid_events','invalid_events','joined_control_events',
        'joined_values_verified_events','missing_control_id_events','unmatched_control_context_events',
        'ambiguous_control_context_events','joined_value_mismatch_events',
        'joined_events_with_nonzero_planned_axis_delta')})
    branches=collections.Counter();windows=collections.Counter();flags_seen=collections.Counter()
    axes=collections.Counter();mismatches=collections.Counter();examples=[]
    identity=('sequence','observed_us','processed_us','wire_frame_id','lock_id','track_id')
    for d in rows:
        if not disturbance_selection_contract(d):
            bad['disturbance_selection_contract']+=1;counts['invalid_events']+=1;continue
        counts['valid_events']+=1;axis='y' if d['axis'] else 'x';axes[axis]+=1
        branches[schema['disturbance_selection_branches'][str(d['branch'])]]+=1
        windows[schema['disturbance_selection_windows'][str(d['window'])]]+=1
        for flag,label in schema['disturbance_selection_event_flags'].items():
            if d['event_flags']&int(flag):flags_seen[label]+=1
        metrics['disturbance_event_selected_velocity_control_pixels_per_second'].append(d['selected_velocity_milli']/1000)
        if not d['event_flags']&1:
            metrics['disturbance_event_absolute_selected_step_control_pixels_per_second'].append(abs(d['selected_velocity_milli']-d['previous_selected_milli'])/1000)
        if d['event_flags']&128:
            metrics['disturbance_event_interval_width_control_pixels_per_second'].append((d['maximum_velocity_milli']-d['minimum_velocity_milli'])/1000)
            metrics['disturbance_event_quality_weight'].append(d['quality_weight_e6']/1e6)
        candidates=[c for c in by_id.get(d['control_trace_id'],[]) if all(c[k]==d[k] for k in identity)]
        matched=None;value_match=None
        if not by_id.get(d['control_trace_id']):counts['missing_control_id_events']+=1
        elif not candidates:counts['unmatched_control_context_events']+=1
        elif len(candidates)!=1:
            counts['ambiguous_control_context_events']+=1;bad['disturbance_selection_ambiguous_core_context']+=1
        else:
            matched=candidates[0];counts['joined_control_events']+=1
            # Legacy v6 has six significant digits, NOT six decimal places.
            # Use its magnitude-dependent half step plus precise-field half
            # quantization; identities and clocks above remain exact.
            checks={
                'proposal':legacy_six_significant_matches(d['proposal_milli'],matched['proposal_'+axis],1000),
                'disturbance':legacy_six_significant_matches(d['disturbance_milli'],matched['disturbance_'+axis],1000),
                'selected_velocity':legacy_six_significant_matches(d['selected_velocity_milli'],matched['velocity_'+axis],1000),
                'current_delay':d['current_delay_us']==matched['response_delay_us'],
                'response':legacy_six_significant_matches(d['response_e6'],matched['response_'+axis],1_000_000),
            }
            dimension=matched['_coordinate_height' if d['axis'] else '_coordinate_width']
            if dimension:
                checks['target_error']=legacy_six_significant_matches(d['target_error_milli'],matched['target_'+axis],1000,dimension/2)
            value_match=all(checks.values())
            if value_match:counts['joined_values_verified_events']+=1
            for label,ok in checks.items():
                if not ok:mismatches[label]+=1
            if not value_match:counts['joined_value_mismatch_events']+=1;bad['disturbance_selection_core_value_mismatch']+=1
            # These are only Core PLANNED deltas, not dispatches or USB/HID.
            if matched['d'+axis]:counts['joined_events_with_nonzero_planned_axis_delta']+=1
        if d['event_flags']&8 and len(examples)<16:
            examples.append({'control_trace_id':d['control_trace_id'],'sequence':d['sequence'],
                'observed_us':d['observed_us'],'axis':axis,
                'branch':schema['disturbance_selection_branches'][str(d['branch'])],
                'proposal_pixels_per_second':d['proposal_milli']/1000,
                'previous_selected_pixels_per_second':d['previous_selected_milli']/1000,
                'selected_pixels_per_second':d['selected_velocity_milli']/1000,
                'interval_pixels_per_second':[d['minimum_velocity_milli']/1000,d['maximum_velocity_milli']/1000],
                'current_delay_us':d['current_delay_us'],'restored_delay_us':d['restored_delay_us'],
                'core_context_joined':matched is not None,'core_values_match':value_match,
                'core_planned_axis_delta':matched['d'+axis] if matched is not None else None})
    return {'event_only':True,'recorded_axis_events':len(rows),**dict(counts),
        'branches_among_retained_valid_events':dict(branches),'windows_among_retained_valid_events':dict(windows),
        'axes_among_retained_valid_events':dict(axes),'event_flags_among_retained_valid_events':dict(flags_seen),
        'joined_value_mismatches':dict(mismatches),'near_target_jump_examples':examples,
        'explicit_lost_events':lost,'reported_reset_count':sum(reset_reports) if reset_reports else None,
        'loss_report_observed':lost is not None,'physical_execution_measured':False,
        'semantics':'Event-only conditional evidence, not a full-frame distribution or continuity proof. No lost report means unknown, not zero loss. Missing/ambiguous Core context remains unverified. selected_velocity is internal feedforward in pixels/second; joined dx/dy are planned counts, neither is dispatch, hardware completion or physical execution.'}


def analyze(collection,output,process=None,since_unix_ms=0):
    output.mkdir(parents=True,exist_ok=True)
    schema=json.loads(Path(__file__).with_name('control-trace-schema.json').read_text(encoding='utf-8'))
    db=sqlite3.connect(output/'numeric-traces.sqlite')
    db.executescript('DROP TABLE IF EXISTS records; CREATE TABLE records(kind TEXT, trace TEXT, row_id INTEGER, stamp INTEGER, pid INTEGER, model_width INTEGER, model_height INTEGER, raw TEXT, signature TEXT, PRIMARY KEY(kind,trace,signature)); CREATE INDEX record_kind_trace ON records(kind,trace);')
    files=[];bad=collections.Counter();duplicates=collections.Counter();losses=collections.defaultdict(collections.Counter)
    events_seen=set();manifests={};costs=collections.defaultdict(list);unknown=collections.Counter()
    disturbance_resets=collections.defaultdict(list)
    probe={}
    probe_path=collection/'projection-probe.json'
    if probe_path.exists():
        try:probe=json.loads(probe_path.read_text(encoding='utf-8'))
        except (ValueError,UnicodeError):bad['invalid_probe_metadata']+=1
    paths=sorted(set(collection.glob('visionforge-mobile-runtime.jsonl*'))|set(collection.glob('host-events*.jsonl*'))|set(collection.glob('projection-probe-trace.jsonl')))
    for path in paths:
        digest=hashlib.sha256();lines=0
        with path.open('rb') as source:
            for data in source:
                digest.update(data);lines+=1
                try:e=json.loads(data.decode('utf-8-sig'))
                except (ValueError,UnicodeError):bad['invalid_or_partial_json_line']+=1;continue
                if 'control_trace' in e or 'pipeline_trace' in e:
                    e={'trace_id':f"projection-probe-{probe.get('process_id',process or 0)}-{probe.get('started_unix_ms',0)}",
                       'process_id':probe.get('process_id',process or 0),'timestamp_unix_ms':e.get('unix_ms',0),
                       'event':'runtime_probe_sample','sequence':e.get('sample'),
                       'detail':e.get('control_trace','')+' '+e.get('pipeline_trace','')}
                if process is not None and e.get('process_id') not in (None,process):continue
                if e.get('timestamp_unix_ms',0)<since_unix_ms:continue
                trace=str(e.get('trace_id','unknown'));event=e.get('event','');detail=e.get('detail','')
                # Legacy events can lack sequence or timestamps; only exact
                # duplicate JSON records are removed, never all missing keys.
                key=hashlib.sha256(json.dumps(e,sort_keys=True,separators=(',',':')).encode()).digest()
                if key in events_seen:duplicates['events']+=1;continue
                events_seen.add(key)
                nums={k:float(v) for k,v in NUMBER.findall(detail[:4000])}
                if event=='runtime_session_manifest':manifests[trace]=detail
                if event=='runtime_probe_sample':
                    manifests[trace]=json.dumps({k:probe.get(k) for k in ('scope','process_id','expected_mode','model','physical_USB_HID_opened_by_session')},sort_keys=True)
                    for k,v in re.findall(r'\b([a-z_]+_lost)=(\d+)',detail):losses[trace][k]+=int(v)
                if event=='runtime_trace_cost':costs[trace].append(nums)
                if event in ('runtime_probe_sample','runtime_trace_batch'):
                    for value in re.findall(r'\bdisturbance_selection_trace_reset=(\d+)',detail):
                        disturbance_resets[trace].append(int(value))
                # Native batch losses are interval counts; Host SPSC losses are cumulative.
                if event=='runtime_trace_batch':
                    for k,v in nums.items():
                        if k.endswith('_lost'):losses[trace][k]+=int(v)
                if event=='host_frame_trace':
                    losses[trace]['host_frame_trace_lost']=max(losses[trace]['host_frame_trace_lost'],int(nums.get('host_frame_trace_lost',0)))
                if event=='runtime_trace_format_error':bad['producer_format_error']+=1
                for name,payload in ARRAY.findall(detail):
                    if name not in FIXED and name not in ('control_trace_v6','detection_trace_v1','image_motion_trace_v1'):
                        unknown[name]+=1;continue
                    for encoded in filter(None,payload.split(';')):
                        try:
                            values=[float(v) if any(c in v.lower() for c in '.e') else int(v) for v in encoded.split(',')]
                            assert all(math.isfinite(v) for v in values)
                            if name in FIXED:assert len(values)==len(schema[FIXED[name]])
                            elif name=='control_trace_v6':assert len(values)==len(BASE_CONTROL)+len(schema['extra_fields'])
                            elif name=='detection_trace_v1':assert len(values)>=12 and 0<=values[11]<=16 and len(values)==12+values[11]*6
                            assert values and values[0]>=0
                            if name=='disturbance_selection_trace_v1':assert all(type(v) is int for v in values)
                        except (ValueError,AssertionError):bad[name+'_malformed']+=1;continue
                        raw=json.dumps(values,separators=(',',':'));signature=hashlib.sha256(raw.encode()).hexdigest()
                        # Exact duplicates are deduplicated. Reused row IDs after a
                        # runtime reset remain distinct when their clocks differ.
                        cursor=db.execute('INSERT OR IGNORE INTO records VALUES(?,?,?,?,?,?,?,?,?)',
                            (name,trace,int(values[0]),int(e.get('timestamp_unix_ms',0)),int(e.get('process_id',0)),
                             int(nums.get('coordinate_width',nums.get('model_width',0))),int(nums.get('coordinate_height',nums.get('model_height',0))),raw,signature))
                        if not cursor.rowcount:duplicates[name]+=1
                if lines%500==0:db.commit()
        files.append({'name':path.name,'bytes':path.stat().st_size,'sha256':digest.hexdigest(),'lines':lines})
    db.commit();sessions={}
    for (trace,) in db.execute('SELECT DISTINCT trace FROM records'):
        counts=dict(db.execute('SELECT kind,COUNT(*) FROM records WHERE trace=? GROUP BY kind',(trace,)))
        metrics=collections.defaultdict(list);outcomes=collections.Counter();control=collections.Counter();host=collections.Counter()
        foreground=collections.defaultdict(lambda:collections.defaultdict(list))
        projection=collections.Counter()
        stored=[(kind,json.loads(raw),width,height) for kind,raw,width,height in db.execute(
            'SELECT kind,raw,model_width,model_height FROM records WHERE trace=?',(trace,))]
        disturbance=disturbance_selection_summary(stored,schema,metrics,bad,
            losses[trace].get('disturbance_selection_trace_lost'),disturbance_resets[trace])
        core_contexts={};context_generations={};context_modes={};projection_rows=[];projection_targets=[]
        for kind,row,width,height in stored:
            if kind=='pipeline_trace_v1':
                d=dict(zip(schema['pipeline_fields'],row))
                context_generations[(d['sequence'],d['wire_frame_id'],d['observed_us'])]=d['generation']
            elif kind=='projection_trace_v1':
                d=dict(zip(schema['projection_fields'],row));projection_rows.append(d)
                if d['event']==1 and d['control_sequence']>0 and d['control_wire_frame_id']>0 and d['control_observed_us']>0:
                    key=(d['control_sequence'],d['control_wire_frame_id'],d['control_observed_us'])
                    context_generations[key]=d['generation'];context_modes[key]=d['mode']
            elif kind=='projection_target_trace_v1':projection_targets.append(dict(zip(schema['projection_target_fields'],row)))
            elif kind=='control_trace_v6':
                d=dict(zip(BASE_CONTROL+schema['extra_fields'],row))
                if d['reason'] not in (1,2,3) and d['sequence']>0 and d['wire_frame_id']>0 and d['processed_us']>=d['observed_us']>0:
                    key=observation_key(d)
                    if key in core_contexts:bad['duplicate_consumed_control_context']+=1
                    else:core_contexts[key]=d
        attempt_targets=collections.defaultdict(list)
        for d in projection_targets:
            if d['event']==1:attempt_targets[projection_key(d)].append(d)
        validated_active=[];active_control_keys=set();mode2_attempts=0;old_source_skips=0
        first=last=None
        for kind,row,width,height in stored:
            if kind=='pipeline_trace_v1':
                d=dict(zip(schema['pipeline_fields'],row));at=d['received_us'];first=at if first is None else min(first,at);last=at if last is None else max(last,at)
                for label,end,start in [('decode_us','decoded_planes_us','received_us'),('preprocess_us','preprocess_finished_us','preprocess_started_us'),('queue_us','inference_started_us','enqueued_us'),('inference_wall_us','inference_finished_us','inference_started_us'),('phone_receive_to_result_us','result_ready_us','received_us'),('estimated_capture_to_control_excluding_network_us','control_finished_us','observed_us')]:
                    if d[start] and d[end]>=d[start]:metrics[label].append(d[end]-d[start])
                    elif d[end]:bad['pipeline_clock_order']+=1
                for field in ('qnn_us','background_us','image_motion_us'):metrics[field].append(d[field])
            elif kind=='pipeline_outcome_v1':outcomes[schema['pipeline_outcomes'].get(str(row[6]),'unknown')]+=1
            elif kind in ('host_frame_trace_v1','host_frame_trace_v2','host_frame_trace_v3'):
                d=dict(zip(schema[FIXED[kind]],row));host[str(d['status'])]+=1
                if d['status']==0 and not d['repeated_content']:
                    for field in ('capture_us','bridge_us','encode_us','publish_us','capture_to_publish_us'):metrics['host_'+field].append(d[field])
                    if kind in ('host_frame_trace_v2','host_frame_trace_v3') and any(row[22:28]):
                        for field in schema['host_frame_v2_fields'][22:]:metrics['host_'+field].append(d[field])
                    if 'foreground_pid' in d:
                        for field in ('capture_to_publish_us','encode_us','mf_readback_us','mf_color_conversion_us'):
                            foreground[str(d['foreground_pid'])][field].append(d[field])
            elif kind=='projection_trace_v1':
                d=dict(zip(schema['projection_fields'],row))
                projection['mode_'+str(d['mode'])+'_event_'+str(d['event'])]+=1
                projection[schema['projection_outcomes'].get(str(d['outcome']),'unknown_outcome')]+=1
                if d['event']==1:
                    if d['math_work_us']>0:metrics['projection_math_work_us'].append(d['math_work_us'])
                    if d['started_us'] and d['finished_us']>=d['started_us']:
                        metrics['projection_total_work_us'].append(d['finished_us']-d['started_us'])
                    if d['b_observed_us']>d['a_observed_us']>0:
                        metrics['projection_available_pixel_age_gain_us'].append(d['b_observed_us']-d['a_observed_us'])
                    if d['outcome']==8:
                        control_key=(d['control_sequence'],d['control_wire_frame_id'],d['control_observed_us'])
                        consumed=core_contexts.get(control_key)
                        evidence=attempt_targets.get(projection_key(d),[])
                        indices=[v['source_index'] for v in evidence]
                        target_invalid=(len(indices)!=len(set(indices)) or any(
                            not 0<=v['source_index']<d['source_count'] or v['valid']!=1 or v['math_rejection']!=0 for v in evidence))
                        if not active_projection_contract(d):
                            bad['projection_active_contract']+=1
                        elif target_invalid:
                            bad['projection_active_target_contract']+=1
                        elif control_key in active_control_keys:
                            bad['projection_duplicate_active_consumption']+=1
                        elif consumed is None:
                            # A snapshot can end after projection drain but
                            # before the next Core drain. Missing is not proof.
                            projection['active_claim_missing_core_evidence']+=1
                        elif consumed['processed_us']<d['finished_us']:
                            bad['projection_active_core_clock_order']+=1
                        else:
                            if len(evidence)!=d['source_count']:projection['active_claim_missing_target_evidence']+=1
                            active_control_keys.add(control_key)
                            validated_active.append(d);projection['validated_consumed_B']+=1
                            metrics['projection_consumed_pixel_age_gain_us'].append(d['b_observed_us']-d['a_observed_us'])
                            metrics['projection_consumed_pixel_age_us'].append(consumed['processed_us']-d['b_observed_us'])
                            metrics['projection_source_semantic_age_at_core_us'].append(consumed['processed_us']-d['a_observed_us'])
                            if d['finished_us']>=d['b_observed_us']:
                                metrics['projection_pixel_age_at_math_finish_us'].append(d['finished_us']-d['b_observed_us'])
                    if d['mode']==2:
                        mode2_attempts+=1
                        if d['outcome']==10:old_source_skips+=1
                elif d['event']==2:
                    if not (d['current_nnp_sequence']==d['b_sequence'] and
                            d['current_nnp_wire_frame_id']==d['b_wire_frame_id'] and
                            d['current_nnp_observed_us']==d['b_observed_us']):
                        bad['projection_comparison_not_same_frame']+=1
                    elif not comparison_contract(d):
                        bad['projection_comparison_counts_or_sentinel']+=1
                elif d['event']==3:projection['unscored_batches']+=1
            elif kind=='projection_target_trace_v1':
                d=dict(zip(schema['projection_target_fields'],row))
                if d['event']==2:
                    projection['comparison_target_rows']+=1
                    if not d['valid']:projection['comparison_unmatched_or_low_iou_targets']+=1
            elif kind=='delivery_trace_v1':
                d=dict(zip(schema['delivery_fields'],row))
                if d['outcome']==1:
                    for label,end,start in [('offer_queue_us','started_us','offered_us'),('offer_to_ack_us','acknowledged_us','offered_us'),('offer_to_completion_us','completed_us','offered_us')]:
                        if d[end]>=d[start]>0:metrics[label].append(d[end]-d[start])
            elif kind=='control_trace_v6':
                d=dict(zip(BASE_CONTROL+schema['extra_fields'],row));flags=int(d['flags'])
                control['observations']+=1
                if flags&1:control['has_target']+=1
                if flags&2:control['held']+=1
                if flags&4:control['switch_events']+=1
                if d['dx'] or d['dy']:control['nonzero_planned_moves']+=1
                if flags&1 and not flags&(2|16):
                    if width and height:metrics['target_error_control_pixels'].append(math.hypot(d['target_x']-width/2,d['target_y']-height/2))
                    else:control['unknown_coordinate_rows']+=1
        # Reconcile each completed projection to exactly one same-B comparison
        # or explicit unscored terminal; do not discard the missing denominators.
        complete={};math_attempts={};terminals=collections.defaultdict(list);duplicate_complete=set()
        for d in projection_rows:
            key=projection_key(d)
            if d['event']==1 and d['math_work_us']>0:
                if 0<d['source_count']<=16:math_attempts.setdefault(key,d)
                else:bad['projection_invalid_math_source_count']+=1
            if d['event']==1 and d['source_count']>0 and d['valid_count']==d['source_count'] and d['math_reason']==0 and d['math_work_us']>0:
                if key in complete:
                    bad['projection_duplicate_complete_attempt']+=1;duplicate_complete.add(key)
                else:complete[key]=d
            elif d['event'] in (2,3):terminals[key].append(d)
        completed_targets=sum(d['source_count'] for d in complete.values())
        compared_batches=compared_targets=reliable_targets=unscored_batches=unscored_targets=pending_batches=pending_targets=0
        terminal_valid_keys=set();valid_comparisons={}
        for key,attempt in complete.items():
            ts=terminals.get(key,[])
            if len(ts)>1:bad['projection_duplicate_terminal']+=len(ts)-1
            # Conflicting terminal records are not an invitation to select the
            # most favorable result. Preserve their source targets as invalid.
            if len(ts)>1 or key in duplicate_complete:continue
            if not ts:
                pending_batches+=1;pending_targets+=attempt['source_count'];continue
            terminal=ts[0]
            if terminal['source_count']!=attempt['source_count']:
                bad['projection_terminal_source_count']+=1;continue
            if terminal['event']==2:
                if not comparison_contract(terminal):continue
                compared_batches+=1;compared_targets+=attempt['source_count'];reliable_targets+=terminal['comparison_matched'];terminal_valid_keys.add(key)
                valid_comparisons[key]=terminal
            elif 14<=terminal['outcome']<=18:
                unscored_batches+=1;unscored_targets+=attempt['source_count'];terminal_valid_keys.add(key)
            else:bad['projection_invalid_unscored_outcome']+=1
        orphan_terminals=sum(len(ts) for key,ts in terminals.items() if key not in complete)
        # Validate unique source-index rows and one-to-one reliable NNP matches
        # within each same-B comparison, without treating low-IoU pairs as hits.
        target_groups=collections.defaultdict(list)
        for d in projection_targets:
            if d['event']==2:target_groups[projection_key(d)].append(d)
        missing_comparison_target_rows=verified_reliable_target_rows=0
        invalid_target_groups=set()
        for key,group in target_groups.items():
            indices=[d['source_index'] for d in group]
            if len(indices)!=len(set(indices)):
                bad['projection_duplicate_comparison_source_index']+=1;invalid_target_groups.add(key)
            accepted=[d for d in group if d['valid']]
            if any(d['valid'] not in (0,1) or d['same_b_nnp_index']<0 or not 500000<=d['iou_e6']<=1_000_000 or d['error_18point_milli']<0 for d in accepted):
                bad['projection_invalid_reliable_target']+=1;invalid_target_groups.add(key)
            choices=[d['same_b_nnp_index'] for d in accepted]
            if len(choices)!=len(set(choices)):
                bad['projection_reliable_nnp_reused']+=1;invalid_target_groups.add(key)
            terminal=valid_comparisons.get(key)
            if terminal:
                if any(not 0<=i<terminal['source_count'] for i in indices) or any(d['same_b_nnp_index']>=terminal['nnp_count'] for d in accepted):
                    bad['projection_comparison_target_index']+=1;invalid_target_groups.add(key)
                if len(set(indices))==terminal['source_count'] and len(accepted)!=terminal['comparison_matched']:
                    bad['projection_comparison_target_count']+=1;invalid_target_groups.add(key)
        for key,terminal in valid_comparisons.items():
            group=target_groups.get(key,[])
            missing_comparison_target_rows+=max(0,terminal['source_count']-len({d['source_index'] for d in group if 0<=d['source_index']<terminal['source_count']}))
            if key in invalid_target_groups:
                compared_batches-=1;compared_targets-=terminal['source_count'];reliable_targets-=terminal['comparison_matched']
                terminal_valid_keys.discard(key);continue
            verified_reliable_target_rows+=sum(bool(d['valid']) for d in group)
            projection['comparison_source_targets']+=terminal['source_count']
            projection['comparison_reliable_targets']+=terminal['comparison_matched']
            if terminal['comparison_matched']:
                metrics['projection_comparison_mean_18point_error_model_pixels'].append(terminal['mean_18point_error_milli']/1000)
            for d in group:
                if d['iou_e6']>=0:metrics['projection_comparison_iou'].append(d['iou_e6']/1e6)
                if d['valid']:metrics['projection_comparison_18point_error_model_pixels'].append(d['error_18point_milli']/1000)
        invalid_targets=completed_targets-compared_targets-unscored_targets-pending_targets
        attempted_targets=sum(d['source_count'] for d in math_attempts.values())
        terminal_report={'attempted_math_batches':len(math_attempts),'attempted_math_source_targets':attempted_targets,
            'complete_fraction_of_attempted_math_batches':len(complete)/len(math_attempts) if math_attempts else None,
            'complete_source_coverage_of_attempted_math_targets':completed_targets/attempted_targets if attempted_targets else None,
            'complete_batches':len(complete),'complete_source_targets':completed_targets,
            'compared_batches':compared_batches,'compared_source_targets':compared_targets,'reliable_targets':reliable_targets,
            'unscored_batches':unscored_batches,'unscored_source_targets':unscored_targets,
            'pending_batches':pending_batches,'pending_source_targets':pending_targets,'orphan_terminal_rows':orphan_terminals,
            'invalid_terminal_batches':len(complete)-compared_batches-unscored_batches-pending_batches,
            'invalid_terminal_source_targets':invalid_targets,
            'missing_comparison_target_rows':missing_comparison_target_rows,
            'verified_reliable_target_rows':verified_reliable_target_rows,
            'invalid_comparison_target_groups':len(invalid_target_groups),
            'conditional_detector_agreement':reliable_targets/compared_targets if compared_targets else None,
            'same_B_comparison_coverage':compared_targets/completed_targets if completed_targets else None,
            'overall_reliable_fraction_including_unscored_pending':reliable_targets/completed_targets if completed_targets else None,
            'overall_reliable_fraction_of_attempted_math_targets':reliable_targets/attempted_targets if attempted_targets else None,
            'semantics':'Agreement uses valid same-B terminal summary counts; per-target missing/contradictory evidence is separate. Completed-source coverage retains unscored, pending and invalid terminals. Attempted-math coverage additionally retains failed/partial/budget-rejected math source targets. Neither is hit rate or identity truth.'}
        consumed_timing=core_timing(core_contexts,context_generations)
        target_contexts={k:d for k,d in core_contexts.items() if int(d['flags'])&1 and not int(d['flags'])&(2|16)}
        target_timing=core_timing(target_contexts,context_generations)
        mode_contexts=collections.defaultdict(dict)
        for key,d in core_contexts.items():
            mode=context_modes.get(key,probe.get('expected_mode','unknown') if probe else 'unknown')
            mode_contexts[str(mode)][key]=d
            metrics['core_consumed_observation_age_us'].append(d['processed_us']-d['observed_us'])
        modes={mode:core_timing(contexts,context_generations) for mode,contexts in mode_contexts.items()}
        active_count=len(validated_active);mode2=modes.get('2',{})
        consumed2=mode2.get('observations',0);core_hz2=mode2.get('effective_processing_hz')
        active_fraction=active_count/consumed2 if consumed2 else None
        publication={'mode2_attempts':mode2_attempts,'old_source_skip_events':old_source_skips,
            'old_source_skip_fraction_of_mode2_attempts':old_source_skips/mode2_attempts if mode2_attempts else None,
            'validated_consumed_B':active_count,'mode2_core_consumed_observations':consumed2,
            'consumed_B_fraction_of_mode2_Core':active_fraction,
            'consumed_B_effective_hz_over_Core_window':active_fraction*core_hz2 if active_fraction is not None and core_hz2 is not None else None,
            'consumed_B_publications_per_Core_measured_second':active_count/mode2['measured_interval_seconds'] if mode2.get('measured_interval_seconds',0)>0 else None,
            'semantics':'B effective Hz is its fraction of accepted mode2 Core observations times the Core effective processing Hz; the separate event rate divides B count by that measured window. Both retain fallback/empty observations. Validated B requires a strict summary and matching accepted Core context; missing target evidence is explicit, contradictory target evidence is rejected. Planned moves and fake/physical dispatches are separate.'}
        sessions[trace]={'records':counts,'manifest':manifests.get(trace),'explicit_lost_records':dict(losses[trace]),
            'quantiles':{k:quantiles(v) for k,v in metrics.items()},'pipeline_outcomes':dict(outcomes),
            'control_observations':dict(control),'host_status_counts':dict(host),
            'disturbance_selection':disturbance,
            'pixel_projection':dict(projection),
            'projection_terminal_accounting':terminal_report,'projection_publications':publication,
            'core_consumed_timing':consumed_timing,'visible_nonheld_nontracking_target_timing':target_timing,
            'core_consumed_timing_by_mode':modes,
            'host_foreground_quantiles':{pid:{k:quantiles(v) for k,v in fields.items()} for pid,fields in foreground.items()},
            'successful_pipeline_fps':(counts.get('pipeline_trace_v1',0)-1)*1e6/(last-first) if first is not None and last>first else None,
            'diagnostic_snapshot_enqueue_us':quantiles([v['total_snapshot_enqueue_us'] for v in costs[trace] if 'total_snapshot_enqueue_us' in v])}
    report={'scope':__doc__,'since_unix_ms':since_unix_ms,'files':files,'sessions':sessions,'parse_or_contract_errors':dict(bad),'duplicates_removed':dict(duplicates),
        'unindexed_array_types':dict(unknown),'database':'numeric-traces.sqlite',
        'limitations':['Phone and Host clocks are summarized separately; unknown network transit is not included in estimated capture age',
                       'Planned movement and firmware echo are not measured physical execution',
                       'Disturbance-selection rows are event-only and may be overwritten; selected_velocity is internal feedforward rather than HID output. Event frequencies and missing/lost context do not establish frame-level stability or physical execution',
                       'Target error includes scene, manual input and detection error; not a labeled hit metric',
                       'Projection comparisons use later NNP boxes for the exact same pixel frame, not human truth or identity labels; unscored and low-IoU rows are reported separately',
                       'Projection pixel-age gains require a matching accepted Core context; the A-based pipeline capture-to-control field still describes neural source A through the worker control phase, not consumed B pixel age',
                       'Projection gains must be read with Core effective Hz, processing/pixel gaps, old-source skips and complete/unscored/pending denominators; they are not faster NNP, physical movement or hit-rate measurements',
                       'Reception gaps and log loss must be assessed before comparing performance; no completeness is inferred from successful rows alone']}
    (output/'session-summary.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8');db.close();return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('collection',type=Path);p.add_argument('--output',type=Path,required=True);p.add_argument('--process',type=int);p.add_argument('--since-unix-ms',type=int,default=0);a=p.parse_args()
    report=analyze(a.collection,a.output,a.process,a.since_unix_ms)
    print(json.dumps({'sessions':len(report['sessions']),'errors':report['parse_or_contract_errors'],'output':str(a.output/'session-summary.json')}))
