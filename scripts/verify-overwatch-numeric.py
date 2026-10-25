from pathlib import Path
import json,numpy as np
import runpy
helpers=runpy.run_path(str(Path(__file__).with_name('evaluate-overwatch.py')))
session,nms=helpers['session'],helpers['nms']
ROOT=Path(__file__).resolve().parents[1]
rows=[]
for name in ['ow_m_320','ow_m_416','ow_m_640','ow_n_640']:
    p=ROOT/'models/overwatch'/name;s=int(name.split('_')[-1]);sess=session(p/'split.onnx')
    cases=[p/'positive_f32_nchw.raw']+[p/f'calibration/{i:03d}.raw' for i in (5,10,14)]
    for i,file in enumerate(cases):
        x=np.fromfile(file,np.float32).reshape(1,3,s,s);ref=sess.run(None,{'images':x})
        d=ROOT/f'audit/overwatch/phone/{name}-numeric/Result_{i}'
        actual=[np.fromfile(d/('output_'+n+'.raw'),np.float32).reshape(r.shape) for n,r in zip(['coordinates','confidences'],ref)]
        mask=np.maximum(ref[1][0,0],actual[1][0,0])>=.20
        rr=nms(ref[0][0],ref[1][0,0]);qq=nms(actual[0][0],actual[1][0,0])
        coord_err=np.abs(ref[0][0][:,mask]-actual[0][0][:,mask])
        rows.append(dict(model=name,case=i,reference_max_confidence=float(ref[1].max()),qnn_max_confidence=float(actual[1].max()),confidence_max_error=float(np.max(np.abs(ref[1]-actual[1]))),selected_anchor_coordinate_max_error=float(coord_err.max()) if coord_err.size else None,reference_detections=len(rr),qnn_detections=len(qq),reference_boxes=rr,qnn_boxes=qq))
report={'backend':'QNN HTP v75 on SM8650','calibration_layout':'NHWC float32 RGB','cases':rows,'limitation':'Four diagnostic images, including calibration overlap; not independent labeled mAP/recall validation. Absence of positive detections is not an accuracy pass.'}
(ROOT/'audit/overwatch/numeric-comparison.json').write_text(json.dumps(report,indent=2))
for name in ['ow_m_320','ow_m_416','ow_m_640','ow_n_640']:
    rs=[r for r in rows if r['model']==name]
    print(name,'max_conf_error=',max(r['confidence_max_error'] for r in rs),'detections=',[(r['reference_detections'],r['qnn_detections']) for r in rs])
