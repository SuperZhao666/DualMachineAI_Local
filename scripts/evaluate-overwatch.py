"""CPU FP32 reference and visual inspection for the supplied OW exports."""
from pathlib import Path
import json, numpy as np, onnxruntime as ort
from PIL import Image, ImageDraw
ROOT=Path(__file__).resolve().parents[1]
def session(p):
    o=ort.SessionOptions(); o.intra_op_num_threads=4; o.inter_op_num_threads=1
    return ort.InferenceSession(str(p),sess_options=o,providers=['CPUExecutionProvider'])
def nms(coords,conf,threshold=.20):
    ids=np.where(conf>=threshold)[0]; ids=ids[np.argsort(conf[ids])[::-1]]
    boxes=coords.T.copy(); boxes[:,:2]-=boxes[:,2:]/2; boxes[:,2:]+=boxes[:,:2]
    keep=[]
    while len(ids):
        i=int(ids[0]); keep.append(i); ids=ids[1:]
        if not len(ids): break
        a=np.maximum(boxes[i,:2],boxes[ids,:2]); b=np.minimum(boxes[i,2:],boxes[ids,2:])
        inter=np.prod(np.maximum(0,b-a),axis=1)
        area=np.prod(boxes[i,2:]-boxes[i,:2]); areas=np.prod(boxes[ids,2:]-boxes[ids,:2],axis=1)
        ids=ids[inter/(area+areas-inter+1e-9)<.45]
    return [(boxes[i].tolist(),float(conf[i])) for i in keep]
def main():
    manifest=json.loads((ROOT/'models/overwatch/manifest.json').read_text(encoding='utf-8'))
    results=[]; sheet=Image.new('RGB',(4*416,4*440),(20,20,20))
    for col,m in enumerate(manifest['models']):
        p=ROOT/'models/overwatch'/m['name']; s=m['size']; split=session(p/'split.onnx'); full=session(p/'float32.onnx')
        cases=[np.fromfile(p/'positive_f32_nchw.raw',np.float32).reshape(1,3,s,s)]
        cases += [np.fromfile(p/f'calibration/{i:03d}.raw',np.float32).reshape(1,3,s,s) for i in (5,10,14)]
        errors=[]; detections=[]
        for row,x in enumerate(cases):
            a,b=split.run(None,{'images':x}); y=full.run(None,{'images':x})[0]
            errors.append(float(np.max(np.abs(np.concatenate([a,b],axis=1)-y))))
            ds=nms(a[0],b[0,0]); detections.append(ds)
            if row==0:
                np.savez(p/'reference.npz',coordinates=a,confidences=b)
            im=Image.fromarray(np.rint(x[0].transpose(1,2,0)*255).astype(np.uint8)).resize((416,416))
            draw=ImageDraw.Draw(im)
            for box,score in ds:
                box=[v*416/s for v in box]; draw.rectangle(box,outline='lime',width=2); draw.text((box[0],box[1]),f'{score:.2f}',fill='white')
            sheet.paste(im,(col*416,row*440)); ImageDraw.Draw(sheet).text((col*416+4,row*440+418),m['name'],fill='white')
        results.append(dict(name=m['name'],fp32_split_max_error=max(errors),detections=detections))
    sheet.save(ROOT/'audit/overwatch/reference-detections.jpg')
    (ROOT/'audit/overwatch/reference.json').write_text(json.dumps(results,indent=2))
    print(json.dumps([{k:v for k,v in r.items() if k!='detections'} for r in results]))
if __name__=='__main__':main()
