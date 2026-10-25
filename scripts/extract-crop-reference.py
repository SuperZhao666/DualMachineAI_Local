"""Offline FP32 detections from an existing encoded game crop, not phone QNN output."""
import argparse,csv,hashlib,json,sys,time
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'build/video-analysis'))
import cv2
import numpy as np
import onnxruntime as ort

p=argparse.ArgumentParser()
p.add_argument('clip',type=Path);p.add_argument('--output',type=Path,required=True)
p.add_argument('--maximum-frames',type=int,default=1000);p.add_argument('--start',type=int,default=0)
a=p.parse_args();a.output.parent.mkdir(parents=True,exist_ok=True)
index=list(csv.DictReader(a.clip.with_suffix('.csv').open()))
options=ort.SessionOptions();options.intra_op_num_threads=2;options.inter_op_num_threads=1
cv2.setNumThreads(1)
model=root/'models/overwatch/ow_m_416/split.onnx'
session=ort.InferenceSession(str(model),sess_options=options,providers=['CPUExecutionProvider'])
cap=cv2.VideoCapture(str(a.clip));assert cap.isOpened()
started=time.monotonic();processed=0;positive=0;crowded=0
with a.output.open('w',newline='') as file:
    writer=csv.writer(file);writer.writerow(['frame_id','captured_us','class_id','confidence','x1','y1','x2','y2'])
    for i,row in enumerate(index):
        ok,frame=cap.read()
        if not ok:break
        if i<a.start:continue
        if processed>=a.maximum_frames:break
        assert frame.shape[:2]==(416,416),frame.shape
        tensor=cv2.cvtColor(frame,cv2.COLOR_BGR2RGB).astype(np.float32).transpose(2,0,1)[None]/255
        coords,scores=session.run(None,{'images':tensor})
        flat=scores.reshape(-1);anchors=np.flatnonzero(flat>=.2);boxes=[];weights=[]
        for n in anchors:
            x,y,w,h=coords[0,:,n];boxes.append([float(x-w/2),float(y-h/2),float(w),float(h)]);weights.append(float(flat[n]))
        chosen=np.asarray(cv2.dnn.NMSBoxes(boxes,weights,.2,.45)).reshape(-1) if boxes else []
        if len(chosen)==0:writer.writerow([row['frame_id'],row['captured_us'],-1,0,0,0,0,0])
        for n in chosen:
            x,y,w,h=boxes[n];writer.writerow([row['frame_id'],row['captured_us'],0,weights[n],x,y,x+w,y+h])
        processed+=1;positive+=len(chosen)>0;crowded+=len(chosen)>=2
        if processed%100==0:
            file.flush();print('frames',processed,'positive',positive,'crowded',crowded,'seconds',round(time.monotonic()-started,1),flush=True)
cap.release()
metadata={'scope':__doc__,'clip':str(a.clip),'model_sha256':hashlib.sha256(model.read_bytes()).hexdigest(),
          'frames':processed,'positive_frames':positive,'multi_person_frames':crowded,'seconds':time.monotonic()-started}
a.output.with_suffix('.json').write_text(json.dumps(metadata,indent=2));print(json.dumps(metadata,indent=2))
