"""Evaluate existing OW nano model as a head candidate on observed game imagery.

No inferred label semantics or automatic promotion: all candidates are saved for visual review.
"""
from pathlib import Path
import json, numpy as np, onnxruntime as ort
from PIL import Image, ImageDraw
root=Path(__file__).resolve().parents[1]
source=root/'audit/control-20261018/video-reference'
out=root/'audit/control-20261018/head-candidates'; out.mkdir(exist_ok=True)
options=ort.SessionOptions(); options.intra_op_num_threads=4; options.inter_op_num_threads=1
sessions={name:ort.InferenceSession(str(root/'models/overwatch'/name/'split.onnx'),
    sess_options=options,providers=['CPUExecutionProvider']) for name in ['ow_m_640','ow_n_640']}
def detect(name,image,threshold=.15):
    side=640; scale=min(side/image.width,side/image.height)
    width,height=round(image.width*scale),round(image.height*scale)
    dx,dy=(side-width)//2,(side-height)//2
    canvas=Image.new('RGB',(side,side),(114,114,114)); canvas.paste(image.resize((width,height),Image.Resampling.BILINEAR),(dx,dy))
    coords,scores=sessions[name].run(None,{'images':np.asarray(canvas,dtype=np.float32).transpose(2,0,1)[None]/255})
    scores=scores.reshape(-1); chosen=[]
    for index in np.argsort(-scores):
        if scores[index]<threshold: break
        cx,cy,w,h=coords[0,:,index]; box=[(cx-w/2-dx)/scale,(cy-h/2-dy)/scale,(cx+w/2-dx)/scale,(cy+h/2-dy)/scale]
        def iou(a,b):
            intersection=max(0,min(a[2],b[2])-max(a[0],b[0]))*max(0,min(a[3],b[3])-max(a[1],b[1]))
            return intersection/max(1,(a[2]-a[0])*(a[3]-a[1])+(b[2]-b[0])*(b[3]-b[1])-intersection)
        if any(iou(box,other['box'])>.45 for other in chosen): continue
        chosen.append({'box':[float(v) for v in box],'confidence':float(scores[index])})
        if len(chosen)==20: break
    return chosen
rows=[]; panels=[]
for file in sorted(source.glob('frame-*.png')):
    original=Image.open(file).convert('RGB'); number=int(file.stem.split('-')[1])
    for edge in [416,640,720]:
        left,top=(original.width-edge)//2,(original.height-edge)//2
        crop=original.crop((left,top,left+edge,top+edge))
        for name in ['ow_m_640','ow_n_640']:
            boxes=detect(name,crop); rows.append({'frame':number,'view':f'center_{edge}','model':name,'detections':boxes})
    bodies=detect('ow_m_640',original,.3)
    for n,body in enumerate(bodies):
        x1,y1,x2,y2=body['box']; margin=max(x2-x1,y2-y1)*.25
        region=(max(0,int(x1-margin)),max(0,int(y1-margin)),min(original.width,int(x2+margin)),min(original.height,int(y2+margin)))
        if region[2]<=region[0] or region[3]<=region[1]: continue
        crop=original.crop(region); heads=detect('ow_n_640',crop)
        rows.append({'frame':number,'view':f'body_roi_{n}','model':'ow_n_640','body':body,'source_region':region,'detections':heads})
        preview=crop.copy(); draw=ImageDraw.Draw(preview)
        for head in heads:
            draw.rectangle(head['box'],outline='lime',width=2)
            draw.text(head['box'][:2],f"{head['confidence']:.2f}",fill='lime',stroke_width=1,stroke_fill='black')
        preview.thumbnail((300,270)); panel=Image.new('RGB',(320,310),(28,28,28)); panel.paste(preview,((320-preview.width)//2,28))
        ImageDraw.Draw(panel).text((8,8),f"frame {number} body {body['confidence']:.2f} N heads {len(heads)}",fill='white')
        panels.append(panel)
    print(number,len(bodies),flush=True)
(out/'detections.json').write_text(json.dumps(rows,indent=2),encoding='utf-8')
for page in range((len(panels)+11)//12):
    sheet=Image.new('RGB',(1280,930),(12,12,12))
    for j,panel in enumerate(panels[page*12:(page+1)*12]): sheet.paste(panel,((j%4)*320,(j//4)*310))
    sheet.save(out/f'body-head-{page+1}.jpg',quality=93)
print('panels',len(panels),flush=True)
