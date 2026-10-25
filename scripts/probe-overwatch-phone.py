"""Execute exact W8A16 candidates on the connected phone's QNN HTP backend."""
from pathlib import Path
import argparse, subprocess, json, shutil
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
SDK=ROOT.parent/'Current/VisionForge_v17_8_47_ui_runtime_observability_hardened/android_inference_benchmark/qnn_sdk/qairt/2.37.1.250807'
DEVICE='/data/local/tmp/vf-overwatch-20261018'
OUT=ROOT/'audit/overwatch/phone'
def run(args,**kwargs):return subprocess.run(args,check=True,**kwargs)
def push(p,d):run(['adb','push',str(p),d],stdout=subprocess.DEVNULL)
def main():
    parser=argparse.ArgumentParser();parser.add_argument('name',choices=['ow_m_320','ow_m_416','ow_m_640','ow_n_640']);parser.add_argument('--stage',action='store_true');a=parser.parse_args()
    OUT.mkdir(exist_ok=True)
    run(['adb','shell','mkdir','-p',DEVICE])
    if a.stage:
        for f in ['libQnnHtp.so','libQnnHtpPrepare.so','libQnnHtpV75Stub.so','libQnnHtpNetRunExtensions.so','libQnnSystem.so']:
            push(SDK/'lib/aarch64-android'/f,DEVICE+'/'+f)
        push(SDK/'lib/hexagon-v75/unsigned/libQnnHtpV75Skel.so',DEVICE+'/libQnnHtpV75Skel.so')
        push(SDK/'bin/aarch64-android/qnn-net-run',DEVICE+'/qnn-net-run')
        (OUT/'htp.json').write_text(json.dumps({'devices':[{'cores':[{'perf_profile':'burst','rpc_control_latency':100}]}]}))
        (OUT/'extensions.json').write_text(json.dumps({'backend_extensions':{'shared_library_path':DEVICE+'/libQnnHtpNetRunExtensions.so','config_file_path':DEVICE+'/htp.json'}}))
        for f in ['htp.json','extensions.json']:push(OUT/f,DEVICE+'/'+f)
    p=ROOT/'models/overwatch'/a.name
    push(p/'android/build'/f'lib{a.name}_w8a16.so',DEVICE+f'/lib{a.name}_w8a16.so')
    lines=[]
    push(p/'positive_u16.raw',DEVICE+f'/{a.name}.raw')
    lines.append(f'images:={DEVICE}/{a.name}.raw')
    size=int(a.name.split('_')[-1])
    for i in [5,10,14]:
        raw=OUT/f'{a.name}-{i}.raw'
        x=np.fromfile(p/f'calibration/{i:03d}.raw',np.float32).reshape(3,size,size).transpose(1,2,0)
        np.rint(np.clip(x,0,1)*65535).astype(np.uint16).tofile(raw)
        push(raw,DEVICE+'/'+raw.name); lines.append('images:='+DEVICE+'/'+raw.name)
    (OUT/f'{a.name}-input.txt').write_text('\n'.join(lines)+'\n')
    push(OUT/f'{a.name}-input.txt',DEVICE+f'/{a.name}-input.txt')
    script=f'''#!/system/bin/sh
set -eu
cd {DEVICE}
export LD_LIBRARY_PATH={DEVICE}:/vendor/lib64
export ADSP_LIBRARY_PATH="{DEVICE};/vendor/lib/rfsa/adsp;/vendor/dsp"
chmod 755 qnn-net-run
./qnn-net-run --model ./lib{a.name}_w8a16.so --backend ./libQnnHtp.so --input_list ./{a.name}-input.txt --output_dir ./{a.name}-numeric --config_file ./extensions.json --use_native_input_files --profiling_level detailed --log_level warn
'''
    f=OUT/f'{a.name}.sh';f.write_text(script,newline='\n');push(f,DEVICE+'/'+f.name)
    with (OUT/f'{a.name}.log').open('w') as log:
        run(['adb','shell','sh',DEVICE+'/'+f.name],stdout=log,stderr=subprocess.STDOUT)
    run(['adb','pull',DEVICE+f'/{a.name}-numeric',str(OUT)],stdout=subprocess.DEVNULL)
    print(a.name+' HTP numeric execution completed')
if __name__=='__main__':main()
