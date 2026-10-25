"""Compare Windows mouse arrivals with the same Host's captured game pixels.

Uses all mouse input, including the player's hand. Does not isolate MAKCU,
prove command execution, or write a calibration. Camera motion is measured
from background features; recoil, translation and optical-flow failures remain
confounders. Prefer the shared PC QPC capture/input clock. Legacy probes fall
back to a wall-clock anchor, which can be affected by system clock corrections.
"""
import argparse, csv, json, sys
from pathlib import Path
root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'build/video-analysis'))
import cv2
import numpy as np
cv2.setNumThreads(1)
p = argparse.ArgumentParser()
p.add_argument('clip', type=Path)
p.add_argument('mouse', type=Path)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
index = list(csv.DictReader(a.clip.with_suffix('.csv').open()))
meta = json.loads(a.clip.with_suffix('.done.json').read_text())
mouse = list(csv.DictReader(a.mouse.open()))
mouse = [r for r in mouse if int(r['flags']) & 1 == 0]
if not mouse:
    raise ValueError('No relative Windows mouse input was recorded; response cannot be fitted')
has_qpc = bool(mouse) and all(r.get('qpc_us') for r in mouse)
mt = np.array([int(r['qpc_us' if has_qpc else 'unix_us']) for r in mouse], dtype=np.int64)
mc = np.vstack([np.zeros(2), np.cumsum([[int(r['dx']), int(r['dy'])] for r in mouse], axis=0)])
wall_clock_offset = int(meta['first_publish_unix_us']) - int(meta['first_publish_us'])
clock_offset = 0 if has_qpc else wall_clock_offset
boxes = {}
for file in a.clip.parent.glob('detections-*.csv'):
    for r in csv.DictReader(file.open()):
        boxes.setdefault(int(r['frame_id']), []).append([float(r[k]) for k in ['x1','y1','x2','y2']])
cap = cv2.VideoCapture(str(a.clip))
previous = None
previous_row = None
flows = []
decoded = 0
while True:
    ok, frame = cap.read()
    if not ok: break
    if decoded >= len(index): raise ValueError('Decoded/index count mismatch')
    row = index[decoded]
    decoded += 1
    sample_us = int(row['captured_us']) + clock_offset
    if sample_us < mt[0] - 200000 or sample_us > mt[-1] + 200000:
        previous = previous_row = None
        continue
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    if previous is not None:
        mask = np.full(gray.shape, 255, np.uint8)
        mask[300:] = 0
        mask[187:230, 187:230] = 0
        for b in boxes.get(int(previous_row['frame_id']), []):
            x1,y1,x2,y2 = b
            cv2.rectangle(mask, (max(0,int(x1)-12),max(0,int(y1)-12)),
                          (min(415,int(x2)+12),min(415,int(y2)+12)), 0, -1)
        pts = cv2.goodFeaturesToTrack(previous, 80, .02, 13, mask=mask, blockSize=7)
        if pts is not None and len(pts) >= 14:
            nxt, status, _ = cv2.calcOpticalFlowPyrLK(previous, gray, pts, None, winSize=(21,21), maxLevel=3)
            back, back_status, _ = cv2.calcOpticalFlowPyrLK(gray, previous, nxt, None, winSize=(21,21), maxLevel=3)
            good = (status[:,0]>0) & (back_status[:,0]>0) & (np.linalg.norm(back[:,0]-pts[:,0],axis=1)<.8)
            delta = nxt[:,0] - pts[:,0]
            if good.sum() >= 10:
                median = np.median(delta[good], axis=0)
                good &= np.linalg.norm(delta-median,axis=1)<1.5
                if good.sum()>=10 and np.ptp(pts[good,0,0])>140 and np.ptp(pts[good,0,1])>90:
                    dx,dy = np.median(delta[good],axis=0)
                    flows.append({'from_us':int(previous_row['captured_us'])+clock_offset,
                        'to_us':int(row['captured_us'])+clock_offset, 'frame_id':int(row['frame_id']),
                        'x':float(dx),'y':float(dy),'support':int(good.sum())})
    previous, previous_row = gray, row
    if decoded % 1000 == 0: print('decoded',decoded,'flow',len(flows),flush=True)
cap.release()
if decoded != len(index): raise ValueError(f'Decoded/index mismatch: {decoded}/{len(index)}')
if len(flows)<100: raise ValueError('Insufficient background samples')
with a.output.with_suffix('.flow.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,list(flows[0]));w.writeheader();w.writerows(flows)
bins=[]
for k in range(3,len(flows),4):
    q=flows[k-3:k+1]
    if any(r['to_us']!=s['from_us'] for r,s in zip(q,q[1:])):continue
    t0,t1=q[0]['from_us'],q[-1]['to_us']
    if not 10000<t1-t0<80000 or t0<mt[0]+40000 or t1>mt[-1]:continue
    bins.append([t0,t1,sum(r['x'] for r in q),sum(r['y'] for r in q)])
b=np.array(bins)
if len(b)<25: raise ValueError('Insufficient simultaneous mouse/frame bins')

def fit(begin,end,axis):
    local=b[(b[:,0]>=begin)&(b[:,1]<=end)]
    results=[]
    if len(local)<20:return results
    # Smooth player translation/recoil is nuisance, not mouse sensitivity.
    tm=(local[:,0]-local[:,0].mean())/1e6
    design_drift=np.column_stack([np.ones(len(tm)),tm,tm*tm])
    response=local[:,2+axis]/((local[:,1]-local[:,0])/1e6)
    # GPU-bound rendering can exceed 40 ms before captured pixels change.
    # A truncated search can mislabel a boundary fit as low sensitivity.
    for delay in range(-8000,96001,1000):
        ii=np.searchsorted(mt,local[:,0]-delay,side='right')
        jj=np.searchsorted(mt,local[:,1]-delay,side='right')
        count=(mc[jj,axis]-mc[ii,axis])/((local[:,1]-local[:,0])/1e6)
        x=count-design_drift@np.linalg.lstsq(design_drift,count,rcond=None)[0]
        y=response-design_drift@np.linalg.lstsq(design_drift,response,rcond=None)[0]
        if np.dot(x,x)<10000 or np.dot(y,y)<10000:continue
        scale=-np.dot(x,y)/np.dot(x,x)
        quality=np.dot(x,y)**2/(np.dot(x,x)*np.dot(y,y))
        if not .05<scale<5:continue
        results.append({'capture_minus_input_us':delay,'px_per_count':float(scale),
                        'r_squared':float(quality),'bins':len(local)})
    return sorted(results,key=lambda r:-r['r_squared'])[:3]
windows=[]
for begin in np.arange(b[0,0],b[-1,1]-2e6,2e6):
    windows.append({'start_us':int(begin),
                    'start_unix_us':int(begin)+(wall_clock_offset if has_qpc else 0),
                    'x':fit(begin,begin+2e6,0),'y':fit(begin,begin+2e6,1)})
report={'scope':__doc__,'clip':str(a.clip),'mouse':str(a.mouse),'decoded_frames':decoded,
        'flow_samples':len(flows),'synchronous_bins':len(b),'relative_mouse_samples':len(mouse),
        'time_basis':'PC_QPC' if has_qpc else 'legacy_wall_clock_anchor',
        'raw_wall_clock_offset_span_us':int(np.ptp([int(r['unix_us'])-int(r['elapsed_us']) for r in mouse])),
        'overall_x':fit(b[0,0],b[-1,1],0),'overall_y':fit(b[0,0],b[-1,1],1),'windows':windows}
for axis in ['x','y']:
    trusted=[w[axis][0] for w in windows if w[axis] and w[axis][0]['r_squared']>=.85]
    report[axis+'_high_agreement_windows']=len(trusted)
    report[axis+'_scale_quantiles']=np.quantile([r['px_per_count'] for r in trusted],[.1,.5,.9]).tolist() if trusted else []
a.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k not in ['windows','scope']},indent=2))
