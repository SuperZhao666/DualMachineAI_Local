"""Render bounded, existing game crops with exact wire-frame control evidence."""
import argparse,csv,json,sys
from pathlib import Path
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'build/video-analysis'))
import cv2,numpy as np
p=argparse.ArgumentParser();p.add_argument('clip',type=Path);p.add_argument('trace',type=Path)
p.add_argument('--min-control-id',type=int,default=0);p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
index=list(csv.DictReader(a.clip.with_suffix('.csv').open()))
ids={int(r['frame_id']):i for i,r in enumerate(index)}
control={int(r['wire_frame_id']):r for r in csv.DictReader(a.trace.open()) if int(r['id'])>=a.min_control_id and int(r['flags'])&3==1}
available=sorted(set(ids)&set(control))
if not available:raise ValueError('No matching live target frames')
chosen=[available[round(i*(len(available)-1)/15)] for i in range(16)]
chosen_indices={ids[f]:f for f in chosen}
panels={};cap=cv2.VideoCapture(str(a.clip));i=0
while True:
 ok,frame=cap.read()
 if not ok:break
 if i in chosen_indices:
  fid=chosen_indices[i];r=control[fid];x,y=float(r['target_x']),float(r['target_y'])
  cv2.rectangle(frame,(round(float(r['box_x1'])),round(float(r['box_y1']))),(round(float(r['box_x2'])),round(float(r['box_y2']))),(0,220,0),1)
  cv2.drawMarker(frame,(208,208),(255,255,255),cv2.MARKER_CROSS,20,1)
  cv2.drawMarker(frame,(round(x),round(y)),(0,0,255),cv2.MARKER_CROSS,16,2)
  panel=np.zeros((476,416,3),np.uint8);panel[:416]=frame
  for k,s in enumerate([f"frame {fid} lock {r['lock_id']} track {r['track_id']}",f"err {x-208:.1f},{y-208:.1f} move {r['dx']},{r['dy']}",f"velocity {float(r['velocity_x']):.0f},{float(r['velocity_y']):.0f} px/s"]):
   cv2.putText(panel,s,(5,433+k*18),cv2.FONT_HERSHEY_SIMPLEX,.43,(240,240,240),1,cv2.LINE_AA)
  panels[fid]=panel
 i+=1
cap.release()
canvas=np.vstack([np.hstack([panels[f] for f in chosen[j:j+4]]) for j in range(0,16,4)])
cv2.imwrite(str(a.output),canvas)
print(json.dumps({'decoded_frames':i,'index_frames':len(index),'matched_target_frames':len(available),'selected':chosen,'output':str(a.output)}))
