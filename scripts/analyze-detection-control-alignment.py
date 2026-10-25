"""Join same-frame detector output and control decisions without guessing identity."""
import argparse, csv, json, math
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--trace', type=Path, required=True)
p.add_argument('--detections', type=Path, nargs='+', required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
detections = {}
for path in a.detections:
    batch = {}
    for row in csv.DictReader(path.open(encoding='utf-8-sig')):
        fid = int(row['frame_id'])
        boxes = batch.setdefault(fid, [])
        if int(row['class_id']) >= 0:
            boxes.append({k: float(row[k]) for k in ['confidence','x1','y1','x2','y2']})
    for fid, boxes in batch.items():
        if fid in detections and detections[fid] != boxes:
            raise ValueError(f'Conflicting detections for wire frame {fid}; check stream generation')
        detections[fid] = boxes

def iou(x, y):
    intersection = max(0, min(x[2],y[2])-max(x[0],y[0])) * max(0,min(x[3],y[3])-max(x[1],y[1]))
    union = max(0,x[2]-x[0])*max(0,x[3]-x[1]) + max(0,y[2]-y[0])*max(0,y[3]-y[1]) - intersection
    return intersection/union if union>0 else 0

matched = []
for r in csv.DictReader(a.trace.open(encoding='utf-8-sig')):
    fid = int(r['wire_frame_id'])
    if fid not in detections: continue
    flags = int(r['flags'])
    reference = [float(r['box_'+k]) for k in ['x1','y1','x2','y2']]
    boxes = detections[fid]
    def overlap(d): return iou(reference,[d[k] for k in ['x1','y1','x2','y2']])
    best = max(boxes,key=overlap) if boxes and flags&1 else None
    matched.append({'id':int(r['id']), 'wire_frame_id':fid, 'observed_us':int(r['observed_us']),
        'lock_id':int(r['lock_id']), 'track_id':int(r['track_id']), 'flags':flags,
        'reason':int(r['reason']), 'detections':len(boxes), 'held':bool(flags&2),
        'overlap_iou':overlap(best) if best else 0,
        'overlap_confidence':best['confidence'] if best else 0,
        'box_center_distance':math.hypot((reference[0]+reference[2]-best['x1']-best['x2'])/2,
            (reference[1]+reference[3]-best['y1']-best['y2'])/2) if best else None,
        'target_x':float(r['target_x']), 'target_y':float(r['target_y']),
        **{k:(best[k] if best else None) for k in ['x1','y1','x2','y2']}})
held = [r for r in matched if r['held']]
overlapping = [r for r in held if r['overlap_iou']>=.5 and r['overlap_confidence']>=.461]
report = {'scope':__doc__, 'detector_frames':len(detections), 'matched_control_frames':len(matched),
    'held_frames':len(held), 'held_without_any_detection':sum(r['detections']==0 for r in held),
    'held_with_strongly_overlapping_confident_detection':len(overlapping),
    'positive_detector_frames':sum(bool(x) for x in detections.values()),
    'review_examples':sorted(overlapping,key=lambda r:r['overlap_iou'],reverse=True)[:12],
    'limitation':'Box overlap is not physical-person identity. Similar positions may belong to neighbours; this is a diagnostic shortlist, not proof a hold was wrong.'}
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
if matched:
    with a.output.with_suffix('.csv').open('w',newline='',encoding='utf-8') as f:
        writer=csv.DictWriter(f,list(matched[0]));writer.writeheader();writer.writerows(matched)
print(json.dumps({k:v for k,v in report.items() if k!='review_examples'},indent=2))
