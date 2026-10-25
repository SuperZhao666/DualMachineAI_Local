"""Check supplied FP32 model response to local video crops and full-frame letterbox."""
from pathlib import Path
import json, numpy as np, onnxruntime as ort
from PIL import Image, ImageDraw
root=Path(__file__).resolve().parents[1]
directory=root/'audit/control-20261018/video-reference'
manifest=json.loads((root/'models/overwatch/manifest.json').read_text(encoding='utf-8'))
results=[]
for model in manifest['models']:
    options=ort.SessionOptions(); options.intra_op_num_threads=4; options.inter_op_num_threads=1
    session=ort.InferenceSession(str(root/'models/overwatch'/model['name']/'split.onnx'),sess_options=options,providers=['CPUExecutionProvider'])
    side=model['size']; frames=[]
    for source in sorted(directory.glob('frame-*.png')):
        original=Image.open(source).convert('RGB')
        crop=original.crop((536,256,744,464)).resize((side,side),Image.Resampling.BILINEAR)
        fit=original.resize((side,round(side*720/1280)),Image.Resampling.BILINEAR)
        full=Image.new('RGB',(side,side),(114,114,114)); full.paste(fit,(0,(side-fit.height)//2))
        row={'frame':int(source.stem.split('-')[1]),'views':{}}
        for name,im in [('center_roi',crop),('full_frame',full)]:
            x=np.asarray(im,dtype=np.float32).transpose(2,0,1)[None]/255
            coords,confidence=session.run(None,{'images':x})
            flat=confidence.reshape(-1); i=int(flat.argmax())
            row['views'][name]={'max_confidence':float(flat[i]),'anchors_above_020':int(np.sum(flat>=.2)),
                'top_xywh':coords[0,:,i].tolist()}
        frames.append(row)
    result={'model':model['name'],'frames':frames}
    results.append(result)
    print(model['name'],{view:sum(r['views'][view]['max_confidence']>=.2 for r in frames) for view in ['center_roi','full_frame']},flush=True)
(directory/'reference.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
