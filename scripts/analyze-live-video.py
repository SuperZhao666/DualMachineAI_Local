"""Frame-by-frame evidence for a recorded game, not a closed-loop acceptance test."""
from pathlib import Path
import sys, csv, json, time, hashlib
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'build/video-analysis'))
import cv2
import numpy as np
import onnxruntime as ort

source=Path(sys.argv[1]);output=Path(sys.argv[2]);output.mkdir(parents=True,exist_ok=True)
options=ort.SessionOptions();options.intra_op_num_threads=4;options.inter_op_num_threads=1
session=ort.InferenceSession(str(root/'models/overwatch/ow_m_416/split.onnx'),sess_options=options,providers=['CPUExecutionProvider'])
capture=cv2.VideoCapture(str(source))
assert capture.isOpened(),source
metadata={'source':str(source),'sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
          'reported_fps':capture.get(cv2.CAP_PROP_FPS),'reported_frames':capture.get(cv2.CAP_PROP_FRAME_COUNT),
          'scope':'offline FP32 RGB center ROI reference; not original phone output; global flow includes player hand movement',
          'video_time_scope':'relative decoded PTS; file creation metadata describes finalization, not precise capture start'}
rows=[];detections=[];previous=None;points=None;previous_pts=None
started=time.monotonic();index=0
mask=np.zeros((360,640),np.uint8);mask[10:240,10:270]=255;mask[10:110,270:570]=255
while True:
    ok,frame=capture.read()
    if not ok:break
    assert frame.shape[:2]==(1440,2560),frame.shape
    pts=capture.get(cv2.CAP_PROP_POS_MSEC)
    small=cv2.resize(frame,(640,360),interpolation=cv2.INTER_AREA)
    gray=cv2.cvtColor(small,cv2.COLOR_BGR2GRAY)
    dx=dy=float('nan');support=0;difference=0.0
    if previous is not None:
        difference=float(np.abs(gray.astype(np.int16)-previous.astype(np.int16)).mean())
        if points is None or len(points)<25 or index%5==0:
            points=cv2.goodFeaturesToTrack(previous,150,.02,10,mask=mask)
        if points is not None:
            after,status,error=cv2.calcOpticalFlowPyrLK(previous,gray,points,None,winSize=(21,21),maxLevel=3)
            good=(status.ravel()==1)&(error.ravel()<35)
            before=points[good];after=after[good]
            if len(before)>=12:
                affine,inliers=cv2.estimateAffinePartial2D(before,after,method=cv2.RANSAC,ransacReprojThreshold=2)
                if affine is not None:
                    center=np.array([320.,180.,1.]);moved=affine@center-center[:2]
                    dx,dy=(moved*4).tolist();support=int(inliers.sum())
            points=after.reshape(-1,1,2)
    previous=gray
    crop=frame[512:928,1072:1488]
    tensor=cv2.cvtColor(crop,cv2.COLOR_BGR2RGB).astype(np.float32).transpose(2,0,1)[None]/255
    coords,scores=session.run(None,{'images':tensor})
    flat=scores.reshape(-1);confident=np.flatnonzero(flat>=.2)
    boxes=[];weights=[]
    for anchor in confident:
        x,y,w,h=coords[0,:,anchor];boxes.append([float(x-w/2),float(y-h/2),float(w),float(h)]);weights.append(float(flat[anchor]))
    selected=cv2.dnn.NMSBoxes(boxes,weights,.2,.45) if boxes else []
    found=[]
    for selected_index in np.asarray(selected).reshape(-1):
        box=boxes[int(selected_index)];score=weights[int(selected_index)]
        found.append({'confidence':score,'xywh':box})
    nearest=min(found,key=lambda d:(d['xywh'][0]+d['xywh'][2]/2-208)**2+(d['xywh'][1]+d['xywh'][3]*.18-208)**2) if found else None
    row={'frame_index':index,'pts_ms':pts,'dt_ms':0 if previous_pts is None else pts-previous_pts,
         'global_dx_fullres':dx,'global_dy_fullres':dy,'flow_inliers':support,'gray_mean_abs_change':difference,
         'max_confidence':float(flat.max()),'detections':len(found),
         'reference_head_error_x':nearest['xywh'][0]+nearest['xywh'][2]/2-208 if nearest else float('nan'),
         'reference_head_error_y':nearest['xywh'][1]+nearest['xywh'][3]*.18-208 if nearest else float('nan')}
    rows.append(row);detections.append({'frame_index':index,'pts_ms':pts,'detections':found})
    previous_pts=pts;index+=1
    if index%100==0:print('frames',index,'video_seconds',round(pts/1000,2),'wall_seconds',round(time.monotonic()-started,1),flush=True)
capture.release()
with (output/'frame-analysis.csv').open('w',newline='',encoding='utf-8') as f:
    writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
(output/'reference-detections.json').write_text(json.dumps(detections),encoding='utf-8')
durations=np.array([r['dt_ms'] for r in rows[1:]])
metadata.update({'decoded_frames':len(rows),'last_pts_ms':rows[-1]['pts_ms'],
                 'interval_ms_p50_p95_p99':np.percentile(durations,[50,95,99]).tolist(),
                 'intervals_over_50ms':int((durations>50).sum()),'intervals_over_100ms':int((durations>100).sum()),
                 'reference_positive_frames':sum(r['detections']>0 for r in rows),
                 'reference_high_confidence_frames':sum(r['max_confidence']>=.4 for r in rows),
                 'analysis_seconds':time.monotonic()-started})
(output/'summary.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in metadata.items() if k not in ('source','scope','video_time_scope')},indent=2))
