"""Validate units, loss accounting, clocks, malformed rows and deduplication."""
from pathlib import Path
import json, math, runpy, struct, tempfile

root=Path(__file__).resolve().parents[1]
module=runpy.run_path(str(root/'scripts/analyze-session-diagnostics.py'))
schema=json.loads((root/'scripts/control-trace-schema.json').read_text(encoding='utf-8'))
with tempfile.TemporaryDirectory(prefix='vf-diagnostics-') as temporary:
    d=Path(temporary);events=[]
    def event(name,detail,**kw):
        row=dict(trace_id='phone',process_id=7,event=name,detail=detail,timestamp_unix_ms=1000+len(events))
        row.update(kw);events.append(row)
    def encoded(name,row):return name+'=['+','.join(map(str,row))+']'
    event('runtime_trace_batch','control_trace_lost=2 pipeline_outcome_lost=3 projection_trace_lost=4 projection_target_trace_lost=5')
    control=[0]*(len(module['BASE_CONTROL'])+len(schema['extra_fields']))
    control[0]=1;control[6:8]=[515,516];control[11]=1
    event('runtime_control_trace_v6','coordinate_width=1024 coordinate_height=1024 '+encoded('control_trace_v6',control))
    pipe=[1,1,77,2,90,100,110,111,120,121,125,135,140,141,145,9,2,1,1,1]
    event('runtime_pipeline_trace_v1',encoded('pipeline_trace_v1',pipe))
    event('runtime_pipeline_outcome_v1',encoded('pipeline_outcome_v1',[1,2,78,2,150,160,6,0]))
    # Same id, different numeric record must survive a producer reset.
    pipe2=pipe.copy();pipe2[2]=78;pipe2[4:15]=[190,200,210,211,220,221,225,235,240,241,245]
    event('runtime_pipeline_trace_v1',encoded('pipeline_trace_v1',pipe2))
    event('runtime_pipeline_trace_v1',encoded('pipeline_trace_v1',pipe))
    event('runtime_delivery_trace_v1',encoded('delivery_trace_v1',[1,2,3,4,100,110,120,150,140,200,1,1,12,50]))
    event('runtime_image_motion_trace_v1',encoded('image_motion_trace_v1',[1,2,3,4,0,0,1,2,.9,.5,4,5]))
    event('runtime_visible_response_trace_v1',encoded('visible_response_trace_v1',[1]*13))
    # Preserve integer monotonic clocks beyond binary64 precision, and keep
    # neural A / independent pixels B / consumed control identities distinct.
    t=2**54
    projection=[1,2,1,3,10,100,t,11,101,t+16000,10,100,t,11,101,t+16000,t-10000,t+50000,t+42000,t+43800,1700,2,2,0,0,8,0,0,-1,-1,43800,27800]
    event('runtime_projection_trace_v1',encoded('projection_trace_v1',projection))
    consumed=control.copy();consumed[0:4]=[2,11,t+16000,t+45000];consumed[28]=101
    event('runtime_control_trace_v6','coordinate_width=1024 coordinate_height=1024 '+encoded('control_trace_v6',consumed))
    following=consumed.copy();following[0:4]=[3,12,t+36000,t+65000];following[28]=102
    event('runtime_control_trace_v6','coordinate_width=1024 coordinate_height=1024 '+encoded('control_trace_v6',following))
    fallback=projection.copy();fallback[0]=4;fallback[4:7]=[12,102,t+36000]
    fallback[7:10]=[0,0,0];fallback[10:13]=[12,102,t+36000];fallback[13:17]=[12,102,t+36000,t+16000]
    fallback[17:26]=[t+86000,0,t+65000,0,2,0,0,0,9]
    event('runtime_projection_trace_v1',encoded('projection_trace_v1',fallback))
    comparison=projection.copy();comparison[0]=2;comparison[2]=2
    comparison[10:13]=[11,101,t+16000];comparison[25:30]=[13,1,2,400000,750]
    event('runtime_projection_trace_v1',encoded('projection_trace_v1',comparison))
    shadow=projection.copy();shadow[0]=5;shadow[1]=1
    shadow[4:17]=[20,200,t+100000,21,201,t+116000,20,200,t+100000,0,0,0,0]
    shadow[17:20]=[t+150000,t+142000,t+143800];shadow[25]=7
    event('runtime_projection_trace_v1',encoded('projection_trace_v1',shadow))
    unscored=shadow.copy();unscored[0]=3;unscored[2]=3;unscored[25]=14
    event('runtime_projection_trace_v1',encoded('projection_trace_v1',unscored))
    pending=shadow.copy();pending[0]=6
    pending[4:13]=[30,300,t+200000,31,301,t+216000,30,300,t+200000]
    pending[17:20]=[t+250000,t+242000,t+243800]
    event('runtime_projection_trace_v1',encoded('projection_trace_v1',pending))
    failed=pending.copy();failed[0]=7
    failed[4:13]=[40,400,t+300000,41,401,t+316000,40,400,t+300000]
    failed[17:23]=[t+350000,t+342000,t+344700,2700,2,1];failed[24:26]=[18,5]
    event('runtime_projection_trace_v1',encoded('projection_trace_v1',failed))
    target=[1,2,2,3,10,100,t,11,101,t+16000,0,1,0,1200,-300,.9e6,250,4,5,0,800000,750]
    event('runtime_projection_target_trace_v1',encoded('projection_target_trace_v1',target))
    low=target.copy();low[0]=2;low[10]=1;low[11]=0;low[20:22]=[400000,99000]
    event('runtime_projection_target_trace_v1',encoded('projection_target_trace_v1',low))
    event('runtime_detection_trace_v1','detection_trace_v1=[1,2,3,4,5,6,1,0,0,3,1,1]')
    # Exact duplicate JSON event should be removed once, including its losses.
    events.append(events[0].copy())
    event('runtime_control_trace_v6','coordinate_width=416 coordinate_height=416 '+encoded('control_trace_v6',control),process_id=8)
    event('runtime_pipeline_trace_v1',encoded('pipeline_trace_v1',pipe),timestamp_unix_ms=1)
    (d/'visionforge-mobile-runtime.jsonl').write_text('\n'.join(json.dumps(e) for e in events)+'\n{partial',encoding='utf-8')
    host=[1,1,50,2,3,0,10,2,3,4,5,40,0,0,0,0,100,1,0,0,0,0]+[1,2,3,4,5,6]
    (d/'host-events.jsonl').write_text(json.dumps(dict(trace_id='host',event='host_frame_trace',timestamp_unix_ms=1100,detail=encoded('host_frame_trace_v2',host))),encoding='utf-8')
    r=module['analyze'](d,d/'out',process=7,since_unix_ms=500)
    p=r['sessions']['phone'];q=p['quantiles']
    assert p['records']['pipeline_trace_v1']==2
    assert q['target_error_control_pixels']['p50']==5
    assert q['phone_receive_to_result_us']['p50']==40
    assert q['offer_queue_us']['p50']==10 and q['offer_to_ack_us']['p50']==40
    assert p['explicit_lost_records']=={'control_trace_lost':2,'pipeline_outcome_lost':3,'projection_trace_lost':4,'projection_target_trace_lost':5}
    assert q['projection_consumed_pixel_age_gain_us']['p50']==16000
    assert q['projection_math_work_us']['p50']==1700
    assert q['projection_comparison_18point_error_model_pixels']['n']==1
    assert q['projection_comparison_18point_error_model_pixels']['p50']==.75
    assert p['pixel_projection']['unscored_batches']==1
    assert p['pixel_projection']['comparison_unmatched_or_low_iou_targets']==1
    assert q['projection_consumed_pixel_age_us']['p50']==29000
    assert q['projection_source_semantic_age_at_core_us']['p50']==45000
    timing=p['core_consumed_timing'];publication=p['projection_publications']
    assert timing['observations']==2 and timing['effective_processing_hz']==50
    assert timing['processed_gap_us']['p95']==20000
    assert publication['mode2_core_consumed_observations']==2
    assert publication['validated_consumed_B']==1 and publication['consumed_B_fraction_of_mode2_Core']==.5
    assert publication['consumed_B_effective_hz_over_Core_window']==25
    accounting=p['projection_terminal_accounting']
    assert accounting['complete_batches']==3 and accounting['complete_source_targets']==6
    assert accounting['compared_source_targets']==2 and accounting['unscored_source_targets']==2 and accounting['pending_source_targets']==2
    assert accounting['conditional_detector_agreement']==.5 and accounting['same_B_comparison_coverage']==1/3
    assert accounting['overall_reliable_fraction_including_unscored_pending']==1/6
    assert accounting['attempted_math_batches']==4 and accounting['attempted_math_source_targets']==8
    assert accounting['complete_fraction_of_attempted_math_batches']==.75
    assert accounting['complete_source_coverage_of_attempted_math_targets']==.75
    assert accounting['overall_reliable_fraction_of_attempted_math_targets']==.125
    assert accounting['verified_reliable_target_rows']==1 and accounting['missing_comparison_target_rows']==0
    db=module['sqlite3'].connect(d/'out/numeric-traces.sqlite')
    saved=json.loads(db.execute("SELECT raw FROM records WHERE kind='projection_trace_v1' AND row_id=1").fetchone()[0])
    assert saved[9]==t+16000 and isinstance(saved[9],int)
    db.close()
    assert p['pipeline_outcomes']=={'preprocessed_superseded':1}
    assert r['sessions']['host']['quantiles']['host_mf_process_output_us']['p50']==5
    assert r['duplicates_removed']=={'events':1,'pipeline_trace_v1':1}
    assert r['parse_or_contract_errors']=={'detection_trace_v1_malformed':1,'invalid_or_partial_json_line':1}
    assert r['unindexed_array_types']=={}
    (d/'host-events.jsonl').write_text(json.dumps(dict(trace_id='host',event='host_frame_trace',timestamp_unix_ms=1100,detail=encoded('host_frame_trace_v3',host+[123]))),encoding='utf-8')
    r=module['analyze'](d,d/'out',process=7,since_unix_ms=500)
    assert r['sessions']['host']['host_foreground_quantiles']['123']['mf_readback_us']['p50']==1

    # False active claims cannot produce age gains without both a valid frozen
    # contract and a corresponding actually accepted Core observation.
    negative=d/'negative';negative.mkdir()
    invalid=projection.copy();invalid[0]=7;invalid[22]=1;invalid[24]=18
    missing=projection.copy();missing[0]=8;missing[4:13]=[40,400,t+300000,41,401,t+316000,40,400,t+300000]
    missing[13:20]=[41,401,t+316000,t+290000,t+350000,t+342000,t+343800]
    nevents=[dict(trace_id='negative',process_id=7,event='runtime_projection_trace_v1',detail=encoded('projection_trace_v1',v)) for v in (invalid,missing)]
    (negative/'visionforge-mobile-runtime.jsonl').write_text('\n'.join(map(json.dumps,nevents)),encoding='utf-8')
    nr=module['analyze'](negative,negative/'out');np=nr['sessions']['negative']
    assert nr['parse_or_contract_errors']=={'projection_active_contract':1}
    assert np['pixel_projection']['active_claim_missing_core_evidence']==1
    assert np['projection_publications']['validated_consumed_B']==0
    assert 'projection_consumed_pixel_age_gain_us' not in np['quantiles']

    # Duplicate terminal rows and two reliable source targets pointing to one
    # NNP index are invalid, never favorably cherry-picked into agreement.
    corrupt=d/'corrupt';corrupt.mkdir()
    duplicate=unscored.copy();duplicate[0]=9
    wrong_target=target.copy();wrong_target[0]=3;wrong_target[10]=1
    cevents=[dict(trace_id='corrupt',event='runtime_projection_trace_v1',detail=encoded('projection_trace_v1',v)) for v in (projection,comparison,shadow,unscored,duplicate)]
    cevents.extend(dict(trace_id='corrupt',event='runtime_projection_target_trace_v1',detail=encoded('projection_target_trace_v1',v)) for v in (target,wrong_target))
    (corrupt/'visionforge-mobile-runtime.jsonl').write_text('\n'.join(map(json.dumps,cevents)),encoding='utf-8')
    cr=module['analyze'](corrupt,corrupt/'out');ca=cr['sessions']['corrupt']['projection_terminal_accounting']
    assert cr['parse_or_contract_errors']['projection_duplicate_terminal']==1
    assert cr['parse_or_contract_errors']['projection_reliable_nnp_reused']==1
    assert ca['compared_source_targets']==ca['reliable_targets']==0
    assert ca['invalid_terminal_source_targets']==4
    assert ca['overall_reliable_fraction_including_unscored_pending']==0

    # Android fake-sink replay emits serialized reports rather than ordinary
    # runtime events. Index its real Core contexts, including zero-rate seconds.
    probe=d/'probe';probe.mkdir()
    (probe/'projection-probe.json').write_text(json.dumps(dict(process_id=77,started_unix_ms=1000,expected_mode=0,scope='fake_sink',physical_USB_HID_opened_by_session=False)),encoding='utf-8')
    probe_controls=[]
    for i,stamp in enumerate((100000,1100000,3100000,4100000),1):
        c=control.copy();c[0:4]=[i,i,stamp,stamp+20000];c[28]=i;probe_controls.append(c)
    detail='coordinate_width=1024 coordinate_height=1024 control_trace_lost=2 '+encoded('control_trace_v6',probe_controls[0])
    detail+=' control_trace_v6=['+';'.join(','.join(map(str,c)) for c in probe_controls[1:])+']'
    (probe/'projection-probe-trace.jsonl').write_text(json.dumps(dict(sample=1,unix_ms=2000,control_trace=detail,pipeline_trace='projection_trace_lost=3')),encoding='utf-8')
    pr=module['analyze'](probe,probe/'out',process=77)
    pp=next(iter(pr['sessions'].values()));pt=pp['core_consumed_timing_by_mode']['0']
    assert pp['explicit_lost_records']=={'control_trace_lost':2,'projection_trace_lost':3}
    assert pt['observations']==4 and pt['effective_processing_hz']==.75
    assert pt['full_one_second_processing_hz']=={'n':3,'p50':1,'p95':1,'p99':1}
    assert pt['processed_gaps_over100ms']==3 and pt['processed_gap_max_us']==2000000
    assert pr['parse_or_contract_errors']=={}

    # The new family is event-only numeric evidence. Join by Core id AND exact
    # context, preserve >2^54 clocks and reused ids, and never turn selected
    # feedforward into an executed mouse movement (all planned deltas are0).
    assert len(module['BASE_CONTROL'])+len(schema['extra_fields'])==65
    assert len(schema['disturbance_selection_fields'])==35
    disturbance=d/'disturbance';disturbance.mkdir()
    control_names=module['BASE_CONTROL']+schema['extra_fields']
    def core_row(identifier,sequence,observed,wire,velocity):
        values={name:0 for name in control_names}
        values.update(id=identifier,sequence=sequence,observed_us=observed,processed_us=observed+42000,
            wire_frame_id=wire,lock_id=9,track_id=11,target_x=513.25,target_y=509.5,flags=1,
            velocity_x=velocity,proposal_x=400,disturbance_x=velocity,
            response_x=.709,response_y=.707,response_delay_us=24000)
        return [values[name] for name in control_names]
    dc1=core_row(41,301,t+20000,555,206)
    dc2=core_row(42,302,t+40000,556,.634225)
    dr1=[1,41,301,t+20000,t+62000,555,9,11,0,2945,2,0,6,t-80000,t+20000,100000,
         0,56000,15,19634,400000,1000000,1000000,400000,206000,206000,1250,0,
         709000,24000,24000,20000,1,0,0]
    dry=dr1.copy();dry[0]=2;dry[8]=1;dry[10:16]=[1,1,4,t-40000,t+20000,60000]
    dry[19:29]=[0,0,0,0,0,0,0,-2500,0,707000]
    dr2=dr1.copy();dr2[0:6]=[3,42,302,t+40000,t+82000,556]
    dr2[9:11]=[2954,3];dr2[13:15]=[t-60000,t+40000]
    dr2[19]=20634;dr2[24:26]=[634,634];dr2[27]=206000
    missing=dr1.copy();missing[0:6]=[4,99,999,t+80000,t+122000,559]
    missing[9:16]=[1,0,0,1,t+80000,t+80000,0]
    missing[16:26]=[0,0,0,0,0,0,0,400000,0,400000]
    missing[27:29]=[0,709000];missing[32]=0
    # A producer reset may reuse ids; the later clocks/identity must match the
    # later Core row, not whichever id41 was indexed first.
    dc_reset=core_row(41,1,t+1020000,777,206)
    dr_reset=dr1.copy();dr_reset[0:6]=[1,41,1,t+1020000,t+1062000,777]
    dr_reset[13:15]=[t+920000,t+1020000]
    devents=[]
    def devent(name,detail):
        devents.append(dict(trace_id='disturbance',process_id=7,event=name,detail=detail,
                            timestamp_unix_ms=2000+len(devents)))
    devent('runtime_trace_batch','disturbance_selection_trace_lost=2 disturbance_selection_trace_reset=1')
    for c in (dc1,dc2,dc_reset):
        devent('runtime_control_trace_v6','coordinate_width=1024 coordinate_height=1024 '+encoded('control_trace_v6',c))
    for row in (dr1,dry,dr2,missing,dr_reset,dr1):
        devent('runtime_disturbance_selection_trace_v1',encoded('disturbance_selection_trace_v1',row))
    devents.append(devents[0].copy())
    (disturbance/'visionforge-mobile-runtime.jsonl').write_text('\n'.join(map(json.dumps,devents)),encoding='utf-8')
    analyzed=module['analyze'](disturbance,disturbance/'out');ds=analyzed['sessions']['disturbance']['disturbance_selection']
    assert analyzed['parse_or_contract_errors']=={} and analyzed['unindexed_array_types']=={}
    assert analyzed['duplicates_removed']=={'events':1,'disturbance_selection_trace_v1':1}
    assert ds['event_only'] and ds['recorded_axis_events']==5 and ds['valid_events']==5
    assert ds['joined_control_events']==ds['joined_values_verified_events']==4 and ds['missing_control_id_events']==1
    assert ds['unmatched_control_context_events']==ds['ambiguous_control_context_events']==0
    assert ds['joined_value_mismatches']=={} and ds['physical_execution_measured'] is False
    assert ds['joined_events_with_nonzero_planned_axis_delta']==0
    assert ds['branches_among_retained_valid_events']=={'proposal_fallback':2,'nearzero':1,'conservative':1,'insufficient':1}
    assert ds['windows_among_retained_valid_events']=={'full':4,'recent_four':1}
    assert ds['explicit_lost_events']==2 and ds['loss_report_observed'] and ds['reported_reset_count']==1
    example=ds['near_target_jump_examples'][0]
    assert example['selected_pixels_per_second']==.634 and example['previous_selected_pixels_per_second']==206
    assert example['interval_pixels_per_second']==[20.634,400] and example['core_context_joined'] and example['core_values_match']
    assert example['core_planned_axis_delta']==0
    db=module['sqlite3'].connect(disturbance/'out/numeric-traces.sqlite')
    saved=[json.loads(raw)for(raw,)in db.execute("SELECT raw FROM records WHERE kind='disturbance_selection_trace_v1' AND row_id=1")]
    assert len(saved)==2 and {row[3]for row in saved}=={t+20000,t+1020000} and all(type(row[3])is int for row in saved)
    db.close()

    # Missing/contradictory evidence stays unverified; do not select a favorable
    # duplicate Core context. Wrong arity/floating numeric fields are rejected.
    broken=d/'disturbance-broken';broken.mkdir()
    value_error=dr1.copy();value_error[0]=10;value_error[25]=700
    unit_error=dr1.copy();unit_error[0]=15;unit_error[26]=2000
    wrong_context=dr1.copy();wrong_context[0]=11;wrong_context[2]=3000
    bad_axis=dr1.copy();bad_axis[0]=12;bad_axis[8]=2
    bad_near=dr2.copy();bad_near[0]=13;bad_near[26]=20000
    floating=dr1.copy();floating[0]=14;floating[3]=float(t+20000)
    conflicting=dc2.copy();conflicting[control_names.index('velocity_x')]=2
    bevents=[]
    for c in (dc1,dc2,conflicting):
        bevents.append(dict(trace_id='broken',event='runtime_control_trace_v6',detail='coordinate_width=1024 coordinate_height=1024 '+encoded('control_trace_v6',c)))
    for row in (value_error,unit_error,wrong_context,bad_axis,bad_near,floating,dr1[:-1],dr2):
        bevents.append(dict(trace_id='broken',event='runtime_disturbance_selection_trace_v1',detail=encoded('disturbance_selection_trace_v1',row)))
    (broken/'visionforge-mobile-runtime.jsonl').write_text('\n'.join(map(json.dumps,bevents)),encoding='utf-8')
    br=module['analyze'](broken,broken/'out');bs=br['sessions']['broken']['disturbance_selection'];errors=br['parse_or_contract_errors']
    assert errors['disturbance_selection_trace_v1_malformed']==2 and errors['disturbance_selection_contract']==2
    assert errors['disturbance_selection_core_value_mismatch']==2 and errors['disturbance_selection_ambiguous_core_context']==1
    assert errors['duplicate_consumed_control_context']==1
    assert bs['joined_values_verified_events']==0 and bs['joined_value_mismatches']=={'selected_velocity':1,'target_error':1}
    assert bs['unmatched_control_context_events']==1 and bs['ambiguous_control_context_events']==1
    assert bs['explicit_lost_events'] is None and not bs['loss_report_observed'] and bs['reported_reset_count'] is None
    assert bs['physical_execution_measured'] is False

    # Legacy ostringstream defaultfloat retains SIX SIGNIFICANT digits. Its
    # rounding at6000px/s can be5 milli, exceeding the previous fixed2 milli.
    # Only numeric serialization comparison gets this bound; ids/clocks and
    # contexts above continue to match exactly.
    matches=module['legacy_six_significant_matches']
    assert matches(6000005,6000,1000) and not matches(6000006,6000,1000)
    assert matches(-6000005,-6000,1000) and not matches(-6000006,-6000,1000)
    # Current production maximum_velocity_pixels_per_second is1200; include
    # the actual maximum and the rounding interval just below that maximum.
    assert matches(1200000,1200,1000) and not matches(1200006,1200,1000)
    assert matches(1199995,1199.99,1000) and not matches(1199996,1199.99,1000)
    assert matches(709263,.709262,1_000_000) and not matches(709264,.709262,1_000_000)
    # A near-zero target error inherits the rounded512px coordinate's bound,
    # rather than the much smaller bound of the error after subtraction.
    assert matches(6,512.005,1000,512) and not matches(7,512.005,1000,512)
    assert matches(0,0,1000) and not matches(1,0,1000)
    rounding=d/'disturbance-rounding';rounding.mkdir();revents=[]
    speeds=(6000.005,-6000.005,1199.9945,-1199.9945,1200,-1200,9999.995,-9999.995)
    for i,speed in enumerate(speeds):
        # Emulate the real FLOAT32 source then C++ precision6 formatting,
        # and the new integer llround milliscale independently.
        source=struct.unpack('<f',struct.pack('<f',speed))[0]
        legacy=float(format(source,'.6g'))
        scaled=int(math.copysign(math.floor(abs(source)*1000+.5),source))
        c=core_row(200+i,500+i,t+2000000+i*20000,900+i,legacy)
        c[control_names.index('proposal_x')]=legacy
        c[control_names.index('lock_id')]=30+i
        row=dr1.copy();row[0:7]=[100+i,200+i,500+i,t+2000000+i*20000,t+2042000+i*20000,900+i,30+i]
        row[10]=3;row[13:15]=[row[3]-100000,row[3]]
        interval=scaled+(20000 if scaled>0 else -20000)
        row[19:21]=[interval,interval];row[23:26]=[scaled,scaled,scaled]
        revents.append(dict(trace_id='rounding',event='runtime_control_trace_v6',detail='coordinate_width=1024 coordinate_height=1024 '+encoded('control_trace_v6',c)))
        revents.append(dict(trace_id='rounding',event='runtime_disturbance_selection_trace_v1',detail=encoded('disturbance_selection_trace_v1',row)))
    (rounding/'visionforge-mobile-runtime.jsonl').write_text('\n'.join(map(json.dumps,revents)),encoding='utf-8')
    rr=module['analyze'](rounding,rounding/'out');rs=rr['sessions']['rounding']['disturbance_selection']
    assert rr['parse_or_contract_errors']=={} and rs['joined_values_verified_events']==len(speeds)
    assert rs['joined_value_mismatches']=={} and rs['physical_execution_measured'] is False
print('PASS diagnostic index: physical units, distinct clock metrics, reset ids, exact dedup, loss, malformed/partial rows, process/time filters, MF stages')
print('PASS event-only disturbance family:35 integer fields, exact Core id/context joins, reused ids, units, loss/unknown, malformed/ambiguous evidence and no physical-output inference')
print('PASS six-significant-digit dynamic numeric bounds:6000.005, configured1200maximum, signs, magnitude boundaries, target-center subtraction and out-of-bound rejection; semantic contexts remain exact')
