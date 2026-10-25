"""Preserve source exports, promote FP16 weights exactly, split mixed YOLO outputs.

Calibration is explicitly an inherited diagnostic corpus, not a labeled validation set.
Run with the QAIRT Python 3.10 environment (onnx, numpy, Pillow, onnxruntime).
"""
from pathlib import Path
import hashlib, json
import numpy as np
import onnx
from onnx import TensorProto, numpy_helper
from PIL import Image, ImageDraw
from split_yolo_outputs import create_yolov8_split_output_model

ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT.parent / 'Current/VisionForge_v17_8_47_ui_runtime_observability_hardened'
OUT = ROOT / 'models/overwatch'
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def promote(model):
    for t in model.graph.initializer:
        if t.data_type == TensorProto.FLOAT16:
            t.CopyFrom(numpy_helper.from_array(numpy_helper.to_array(t).astype(np.float32), t.name))
    for v in list(model.graph.input)+list(model.graph.output)+list(model.graph.value_info):
        if v.type.tensor_type.elem_type == TensorProto.FLOAT16:
            v.type.tensor_type.elem_type = TensorProto.FLOAT
    for node in model.graph.node:
        for a in node.attribute:
            if a.type == onnx.AttributeProto.TENSOR and a.t.data_type == TensorProto.FLOAT16:
                a.t.CopyFrom(numpy_helper.from_array(numpy_helper.to_array(a.t).astype(np.float32), a.t.name))
            if node.op_type == 'Cast' and a.name == 'to' and a.i == TensorProto.FLOAT16:
                a.i = TensorProto.FLOAT
    # The half exports put I/O casts after their consumers. After promotion those
    # casts are identities; remove them and restore a valid topological order.
    for node in list(model.graph.node):
        if node.op_type == 'Cast' and node.name.startswith('graph_'):
            source, target = node.input[0], node.output[0]
            if any(v.name == target for v in model.graph.output):
                for producer in model.graph.node:
                    for i,name in enumerate(producer.output):
                        if name == source: producer.output[i] = target
            else:
                for consumer in model.graph.node:
                    for i,name in enumerate(consumer.input):
                        if name == target: consumer.input[i] = source
            model.graph.node.remove(node)
    pending=list(model.graph.node); ordered=[]
    available={t.name for t in model.graph.initializer}|{v.name for v in model.graph.input}
    while pending:
        ready=[n for n in pending if all(not x or x in available for x in n.input)]
        if not ready: raise ValueError('Model contains unresolved graph dependencies')
        for n in ready:
            ordered.append(n); available.update(n.output); pending.remove(n)
    del model.graph.node[:]; model.graph.node.extend(ordered)
    return model

def main():
    rows=[]
    cal = OLD/'android_inference_benchmark/qnn_workspace/ow2_416_static_decode_w8a16/calibration/raw'
    known = OLD/'analysis_output/android_production_release_hid_semantics_20260728_151607/qnn_numeric_diagnostic/known_positive_rgb_u16_nhwc.raw'
    images=[]
    for p in sorted(cal.glob('*.raw')):
        a=np.fromfile(p,np.float32).reshape(3,416,416).transpose(1,2,0)
        images.append(Image.fromarray(np.clip(np.rint(a*255),0,255).astype(np.uint8)))
    positive=Image.fromarray((np.fromfile(known,np.uint16).reshape(416,416,3)//257).astype(np.uint8))
    positive.save(ROOT/'audit/overwatch/inherited-positive.png')
    sheet=Image.new('RGB',(5*208,3*208))
    for i,im in enumerate(images): sheet.paste(im.resize((208,208)),((i%5)*208,(i//5)*208))
    sheet.save(ROOT/'audit/overwatch/inherited-calibration.jpg')
    for p in sorted((OUT/'source').rglob('*.onnx')):
        model=onnx.load(p)
        size=model.graph.input[0].type.tensor_type.shape.dim[2].dim_value
        name='ow_m_'+str(size) if 'yang' in p.name else 'ow_n_640'
        d=OUT/name; d.mkdir(exist_ok=True)
        full=d/'float32.onnx'; onnx.save(promote(model),full)
        split=d/'split.onnx'
        anchors=(size//8)**2+(size//16)**2+(size//32)**2
        create_yolov8_split_output_model(full,split,anchors=anchors,classes=1)
        c=d/'calibration'; c.mkdir(exist_ok=True)
        lines=[]
        for i,im in enumerate(images):
            a=np.asarray(im.resize((size,size),Image.Resampling.BILINEAR),np.float32)/255
            raw=c/f'{i:03d}.raw'; a.transpose(2,0,1).tofile(raw)
            lines.append('images:='+str(raw.resolve()))
        (c/'input_list.txt').write_text('\n'.join(lines)+'\n')
        # QNN calibrates the converted NHWC graph. NCHW is only the ONNX input
        # layout declaration; calibration byte order must match the QNN graph.
        q=d/'calibration_nhwc'; q.mkdir(exist_ok=True); lines=[]
        for raw in sorted(c.glob('*.raw')):
            target=q/raw.name
            np.fromfile(raw,np.float32).reshape(3,size,size).transpose(1,2,0).tofile(target)
            lines.append('images:='+str(target.resolve()))
        (q/'input_list.txt').write_text('\n'.join(lines)+'\n')
        a=np.asarray(positive.resize((size,size),Image.Resampling.BILINEAR),np.uint8)
        (a.astype(np.uint16)*257).tofile(d/'positive_u16.raw')
        (a.astype(np.float32)/255).transpose(2,0,1).tofile(d/'positive_f32_nchw.raw')
        (a.astype(np.float32)/255).tofile(d/'positive_f32_nhwc.raw')
        rows.append(dict(name=name,size=size,anchors=anchors,classes=1,source=str(p.relative_to(ROOT)),source_sha256=sha(p),split_sha256=sha(split),split_bytes=split.stat().st_size,metadata={m.key:m.value for m in model.metadata_props}))
    manifest=dict(models=rows,calibration_source=str(cal),calibration_role='inherited diagnostic corpus; not labeled OW validation',positive_source=str(known),archive_sha256=sha(Path(r'C:\Users\Administrator\Downloads\ow等4个文件.rar')))
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding='utf-8')
    print(json.dumps(rows,indent=2,ensure_ascii=False))
if __name__=='__main__': main()
