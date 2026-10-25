"""Offline image-motion / accepted-report correlation. No physical ACK claim."""
import argparse,csv,json,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1];sys.path.insert(0,str(root/'build/video-analysis'))
import cv2,numpy as np
cv2.setNumThreads(1)
p=argparse.ArgumentParser();p.add_argument('clip',type=Path);p.add_argument('trace',type=Path)
p.add_argument('--first',type=int,default=2200);p.add_argument('--last',type=int,default=3400);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();rows=list(csv.DictReader(a.trace.open()))
control={int(r['wire_frame_id']):r for r in rows if int(r['id'])>=800}
index=list(csv.DictReader(a.clip.with_suffix('.csv').open()))
commands={int(r['accepted_at_us']):(int(r['accepted_dx']),int(r['accepted_dy'])) for r in rows if int(r['accepted_at_us'])}
commands=sorted((t,*v) for t,v in commands.items())
times=np.array([c[0] for c in commands]);cx=np.r_[0,np.cumsum([c[1] for c in commands])];cy=np.r_[0,np.cumsum([c[2] for c in commands])]
det={}
for path in a.clip.parent.glob('detections-*.csv'):
 for r in csv.DictReader(path.open()):det.setdefault(int(r['frame_id']),[]).append([float(r[k]) for k in ['x1','y1','x2','y2']])
cap=cv2.VideoCapture(str(a.clip));prev=None;last_row=None;flow=[];i=0
while True:
 ok,im=cap.read()
 if not ok:break
 fid=int(index[i]['frame_id']);i+=1
 if fid<a.first or fid not in control:continue
 if fid>a.last:break
 r=control[fid];gray=cv2.cvtColor(im,cv2.COLOR_BGR2GRAY)
 if prev is not None:
  mask=np.ones(gray.shape,np.uint8)*255;mask[310:]=0;mask[185:232,185:232]=0
  for b in det.get(int(last_row['wire_frame_id']),[])+[[float(last_row[k]) for k in ['box_x1','box_y1','box_x2','box_y2']]]:
   x1,y1,x2,y2=b;cv2.rectangle(mask,(max(0,int(x1)-8),max(0,int(y1)-8)),(min(415,int(x2)+8),min(415,int(y2)+8)),0,-1)
  pts=cv2.goodFeaturesToTrack(prev,70,.02,12,mask=mask,blockSize=7)
  if pts is not None and len(pts)>=10:
   nxt,status,_=cv2.calcOpticalFlowPyrLK(prev,gray,pts,None,winSize=(21,21),maxLevel=3,criteria=(cv2.TERM_CRITERIA_COUNT|cv2.TERM_CRITERIA_EPS,20,.02))
   back,back_status,_=cv2.calcOpticalFlowPyrLK(gray,prev,nxt,None,winSize=(21,21),maxLevel=3)
   good=(status[:,0]>0)&(back_status[:,0]>0)&(np.linalg.norm(back[:,0]-pts[:,0],axis=1)<1.2)
   delta=nxt[:,0]-pts[:,0];med=np.median(delta[good],axis=0) if sum(good) else np.zeros(2)
   good &= np.linalg.norm(delta-med,axis=1)<2.5
   if sum(good)>=8 and np.ptp(pts[good,0,0])>100 and np.ptp(pts[good,0,1])>80:
    dx,dy=np.median(delta[good],axis=0)
    flow.append({'from_us':int(last_row['observed_us']),'to_us':int(r['observed_us']),'frame_id':fid,'x':float(dx),'y':float(dy),'support':int(sum(good))})
 prev=gray;last_row=r
cap.release()
with a.output.open('w',newline='') as f:
 w=csv.DictWriter(f,list(flow[0]));w.writeheader();w.writerows(flow)
report={'samples':len(flow),'first':a.first,'last':a.last,'fits':{}}
for axis,cumulative in [('x',cx),('y',cy)]:
 fits=[]
 for delay in range(4000,120001,2000):
  # Four valid contiguous observations per bin, no fill across gaps.
  xs=[];ys=[]
  for k in range(3,len(flow),4):
   chunk=flow[k-3:k+1]
   if any(b['from_us']!=aa['to_us'] for aa,b in zip(chunk,chunk[1:])):continue
   t0,t1=chunk[0]['from_us'],chunk[-1]['to_us']
   if not 0<t1-t0<80000:continue
   count=cumulative[np.searchsorted(times,t1-delay,side='right')]-cumulative[np.searchsorted(times,t0-delay,side='right')]
   n=28000/(t1-t0);xs.append(count*n);ys.append(sum(q[axis] for q in chunk)*n)
  x=np.asarray(xs);y=np.asarray(ys)
  if len(x)<12 or np.var(x)<1:continue
  xx=x-x.mean();yy=y-y.mean();slope=np.dot(xx,yy)/np.dot(xx,xx);quality=np.dot(xx,yy)**2/(np.dot(xx,xx)*np.dot(yy,yy)+1e-12)
  fits.append({'delay_us':delay,'response':float(-slope),'r2':float(quality),'bins':len(x)})
 report['fits'][axis]=sorted(fits,key=lambda q:-q['r2'])[:8]
a.output.with_suffix('.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
