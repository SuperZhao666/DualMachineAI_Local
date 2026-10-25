"""Render bounded, existing game crops with exact wire-frame control evidence."""
import argparse,csv,json,sys
from pathlib import Path
from control_trace_geometry import axis_dimension
root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'build/video-analysis'))
import cv2,numpy as np
p=argparse.ArgumentParser();p.add_argument('clip',type=Path);p.add_argument('trace',type=Path)
p.add_argument('--min-control-id',type=int,default=0);p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
index=list(csv.DictReader(a.clip.with_suffix('.csv').open()))
ids={int(r['frame_id']):i for i,r in enumerate(index)}
control={int(r['wire_frame_id']):r for r in csv.DictReader(a.trace.open()) if int(r['id'])>=a.min_control_id and int(r['flags'])&(1|2|16)==1}
available=sorted(set(ids)&set(control))
if not available:raise ValueError('No matching live target frames')
chosen=[available[round(i*(len(available)-1)/15)] for i in range(16)]
chosen_indices={ids[f]:f for f in chosen}
panels={};cap=cv2.VideoCapture(str(a.clip));i=0;video_geometry=None;coordinate_geometries=set()
if not cap.isOpened():raise ValueError('Could not open video evidence')
while True:
 ok,frame=cap.read()
 if not ok:break
 if i in chosen_indices:
  fid=chosen_indices[i];r=control[fid];x,y=float(r['target_x']),float(r['target_y'])
  height,width=frame.shape[:2]
  if video_geometry is None:video_geometry=(width,height)
  if video_geometry!=(width,height):raise ValueError('Video changes dimensions; refusing implicit reshape/crop')
  cw,ch=axis_dimension(r,'x'),axis_dimension(r,'y');coordinate_geometries.add((cw,ch))
  sx,sy=width/cw,height/ch
  cv2.rectangle(frame,(round(float(r['box_x1'])*sx),round(float(r['box_y1'])*sy)),(round(float(r['box_x2'])*sx),round(float(r['box_y2'])*sy)),(0,220,0),1)
  cv2.drawMarker(frame,(round(cw/2*sx),round(ch/2*sy)),(255,255,255),cv2.MARKER_CROSS,20,1)
  cv2.drawMarker(frame,(round(x*sx),round(y*sy)),(0,0,255),cv2.MARKER_CROSS,16,2)
  panel=np.zeros((height+60,width,3),np.uint8);panel[:height]=frame
  for k,s in enumerate([f"frame {fid} lock {r['lock_id']} track {r['track_id']}",f"err {x-cw/2:.1f},{y-ch/2:.1f} control px; move {r['dx']},{r['dy']} counts",f"velocity {float(r['velocity_x']):.0f},{float(r['velocity_y']):.0f} control px/s"]):
   cv2.putText(panel,s,(5,height+17+k*18),cv2.FONT_HERSHEY_SIMPLEX,.43,(240,240,240),1,cv2.LINE_AA)
  panels[fid]=panel
 i+=1
cap.release()
if any(f not in panels for f in chosen):raise ValueError('Video/index mismatch: selected frame was not decoded')
canvas=np.vstack([np.hstack([panels[f] for f in chosen[j:j+4]]) for j in range(0,16,4)])
a.output.parent.mkdir(parents=True,exist_ok=True)
if not cv2.imwrite(str(a.output),canvas):raise IOError('Could not write rendered evidence')
print(json.dumps({'decoded_frames':i,'index_frames':len(index),'matched_target_frames':len(available),'selected':chosen,'output':str(a.output),
 'video_geometry':video_geometry,'control_geometries':sorted(coordinate_geometries),'canvas_width':canvas.shape[1],'canvas_height':canvas.shape[0],
 'error_units':'control_coordinate_pixels','overlay':'per-axis control-to-decoded-video scaling; decoded video pixels not resized',
 'scope':'Recorded control proposals are computed after source capture; this overlay is not physical execution at the shown frame.'}))
