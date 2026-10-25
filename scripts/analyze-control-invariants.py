"""Audit coordinate-bound actionable trace proposals, not physical aim accuracy."""
import argparse,csv,json
from pathlib import Path
from control_trace_geometry import center
p=argparse.ArgumentParser(); p.add_argument('traces',type=Path,nargs='+'); p.add_argument('--output',type=Path,required=True)
args=p.parse_args(); out={}
for path in args.traces:
    rows=list(csv.DictReader(path.open(encoding='utf-8')))
    for row in rows:
        center(row,'x'); center(row,'y')
    active=[r for r in rows if int(r['flags'])&(1|2|16)==1]
    axes={}
    for axis in ['x','y']:
        zero=[]; opposite=[]; unsupported=[]
        for r in active:
            velocity,prediction,delta=map(float,[r['velocity_'+axis],r['predicted_error_'+axis],r['d'+axis]])
            error=float(r['target_'+axis])-center(r,axis)
            if velocity==0 and prediction==0 and delta!=0: zero.append(int(r['id']))
            if velocity==0 and prediction*delta<0: opposite.append(int(r['id']))
            if abs(error)>3 and error*delta<0 and error*velocity>=0: unsupported.append(int(r['id']))
        axes[axis]={'move_at_zero_prediction_and_velocity':len(zero),'opposite_prediction_with_zero_velocity':len(opposite),
                    'opposite_observed_error_without_reverse_velocity':len(unsupported),'sample_ids':(zero+opposite)[:20]}
    out[str(path)]={'frames':len(rows),'visible_target_frames':len(active),'axes':axes,
        'held_frames':sum(bool(int(r['flags'])&2)for r in rows),
        'tracking_only_frames':sum(bool(int(r['flags'])&16)for r in rows),
        'scope':'Actionable non-held, non-tracking-only proposals. Per-row control-coordinate centers; missing dimensions use documented legacy416. Not physical execution ground truth.'}
args.output.write_text(json.dumps(out,indent=2),encoding='utf-8'); print(json.dumps(out,indent=2))
