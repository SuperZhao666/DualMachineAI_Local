"""Version, identity, raw-preservation and configured-aim offline contracts."""
from pathlib import Path
import ctypes, json, sys
from control_trace_flags import ControlFlagInterpreter

if sys.platform=='win32':ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(),0x4000)
schema=json.loads(Path(__file__).with_name('control-trace-schema.json').read_text(encoding='utf-8'))
checks=0
def require(value,name):
    global checks
    checks+=1
    if not value:raise AssertionError(name)

def manifest(trace,code,pid=7):
    return {'process_id':pid,'trace_id':trace,'event':'runtime_session_manifest','sequence':1,
            'monotonic_us':80_000,'detail':f'app_version=fixture version_code={code}'}
def profile(trace,aim='head',at=90_000,model='ow_m_640',applied=True):
    return {'process_id':7,'trace_id':trace,'event':'mobile_aim_target_applied','sequence':at,
            'monotonic_us':at,'detail':f'model={model} aim_target={aim} native_applied={str(applied).lower()}'}
def row(trace,flags,at=130_000):return {'trace_id':trace,'flags':flags,'processed_us':at}
def summary(events,rows,override='auto'):
    interpreter=ControlFlagInterpreter(schema,7,override)
    for event in events:interpreter.consume(event)
    return interpreter.summarize(rows)

for code in (38,39):
    s=summary([manifest('head',code),profile('head',model='ow_m_416')],
              [row('head',1|32768),row('head',1|16384)])
    require(s['actionable_head_sources']=={'measured':1,'projected':1},'retained head marker layout')
    require(s['large_background_motion_recovered_frames']==0,'old projected head is never background')
    require(s['configured_geometric_head_frames']==0,'old aliased model token is not geometric')

for code in (49,50,56,59,60,61,62,65,66,67,68):
    s=summary([manifest('body',code),profile('body')],[row('body',1|8|16384),row('body',1)])
    require(s['actionable_head_sources']=={'measured':0,'projected':0},'large background is not head marker')
    require(s['large_background_motion_recovered_frames']==1,'background bit retained')
    require(s['configured_geometric_head_frames']==2,'explicit preceding body-head profile retained')
    require(s['raw_bit_counts']['all_rows']=={'16384':1,'32768':0},'raw bit count preserved')
    if code==50:require(s['trace_layouts'][0]['evidence_level']=='reviewed_release_range','range provenance labelled')
    if code in (60,61,62,65,66,67,68):require(s['trace_layouts'][0]['evidence_level']=='direct_retained_source','current producer is verified explicitly')

for events in ([],[manifest('unknown',63)],[manifest('unknown',64)],[manifest('unknown',999)],[manifest('unknown','bad')],
               [manifest('unknown',39),manifest('unknown',60)]):
    s=summary(events,[row('unknown',1|16384)])
    require(s['actionable_head_sources']=={'measured':None,'projected':None},'unknown semantics is null, not zero')
    require(s['large_background_motion_recovered_frames'] is None,'unknown background total is null')
    require(s['unresolved_rows']==1 and s['unresolved_set_bit_counts']['16384']==1,'unknown raw bit is auditable')

events=[manifest('old',39),manifest('new',60),manifest('old',60,pid=8),profile('new')]
rows=[row('old',1|32768),row('old',1|16384),row('new',1|8|16384)]
snapshot=[dict(r) for r in rows]
s=summary(events,rows)
require(s['actionable_head_sources']=={'measured':1,'projected':1},'mixed input is classified per trace, never maximum version')
require(s['large_background_motion_recovered_frames']==1,'mixed background stays separate')
require(s['configured_geometric_head_frames']==1,'mixed geometry stays separate')
require(rows==snapshot,'interpreter never changes raw row or flags')
layouts={r['trace_id']:r['layout'] for r in s['trace_layouts']}
require(layouts=={'new':'large_background_v1','old':'head_observation_v1'},'another process cannot overwrite trace identity')

s=summary(events,[*rows,row('missing',1|16384)])
require(s['actionable_head_sources']=={'measured':None,'projected':None},'mixed unknown does not become false zero or partial total')
require(s['known_interpreted_actionable_head_sources']=={'measured':1,'projected':1},'known partial counts are labelled')
require(s['unresolved_active_rows']==1,'mixed unknown count remains visible')

s=summary([manifest('oldbody',40)],[row('oldbody',1|16384)])
require(s['actionable_head_sources']['projected'] is None and s['unexpected_or_conflicting_marker_rows']==1,
        'unassigned historical marker is not guessed')
s=summary([manifest('head',39)],[row('head',1|16384|32768)])
require(s['actionable_head_sources']['measured'] is None,'contradictory historical markers stay unresolved')
s=summary([],[row('explicit',1|16384)],'head_observation_v1')
require(s['actionable_head_sources']=={'measured':0,'projected':1} and s['trace_layouts'][0]['resolution']=='explicit_cli_override',
        'caller override is explicit and traceable')

events=[manifest('switch',60),profile('switch',at=80_000),profile('switch','body',120_000)]
s=summary(events,[row('switch',1,100_000),row('switch',1,130_000)])
require(s['active_aim_source_evidence']=={'configured_geometric_head':1,'configured_body_center':1},'profile timeline uses preceding configuration')
events.append(profile('switch','head',140_000,applied=False))
s=summary(events,[row('switch',1,100_000),row('switch',1,130_000),row('switch',1,150_000)])
require(s['configured_geometric_head_frames'] is None and s['active_aim_source_evidence']['unresolved']==1,
        'failed later configuration does not silently reuse stale profile')
s=summary([manifest('noaim',60)],[row('noaim',1|8|16384)])
require(s['configured_geometric_head_frames'] is None,'neither bit nor default ratio invents geometric source')
require(s['actionable_head_sources']=={'measured':0,'projected':0},'head markers remain layout-resolved independently of missing aim profile')
print(f'PASS control flag semantics checks={checks}; raw preservation,38/39 head,49..62 background,unknown null,mixed identities,profile timeline')
