"""Run the production NV12 scalar/NEON parity regression as low-priority synthetic CPU work."""
from pathlib import Path
import argparse,datetime,hashlib,json,subprocess
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--device',required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output.resolve();assert out.is_relative_to((root/'audit').resolve());out.mkdir(parents=True,exist_ok=True)
source=root/'dual_machine_runtime/tests/native_nv12_parity_tests.cpp'
compiler=root.parent/'.android-sdk/ndk/27.0.12077973/toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android29-clang++.cmd'
binary=out/'native-nv12-parity'
argv=[str(compiler),'-std=c++20','-O2','-UNDEBUG','-static-libstdc++','-I'+str(root/'dual_machine_runtime/shared/include'),str(source),'-o',str(binary)]
r=subprocess.run(argv,capture_output=True,text=True,encoding='utf-8',errors='replace',creationflags=subprocess.CREATE_NO_WINDOW|subprocess.BELOW_NORMAL_PRIORITY_CLASS)
(out/'build.txt').write_text(r.stdout+r.stderr,encoding='utf-8');assert r.returncode==0,r.stdout+r.stderr
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
adb=[str(root.parent/'.android-sdk/platform-tools/adb.exe'),'-s',a.device]
remote='/data/local/tmp/vf-pixels-'+sha(binary)[:16]
for command in (adb+['push',str(binary),remote],adb+['shell','chmod','700',remote]):subprocess.run(command,capture_output=True,check=True,creationflags=subprocess.CREATE_NO_WINDOW)
r=subprocess.run(adb+['shell','nice','-n','15',remote],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=20,creationflags=subprocess.CREATE_NO_WINDOW)
(out/'output.txt').write_text(r.stdout+r.stderr,encoding='utf-8')
passed=r.returncode==0 and 'mismatched_bytes=0 guard_failures=0'in r.stdout
record={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'passed':passed,'returncode':r.returncode,'stdout':r.stdout,'source_sha256':sha(source),'kernel_sha256':sha(root/'dual_machine_runtime/shared/src/image_preprocessor.cpp'),'binary_sha256':sha(binary),'app_or_host_started':False,'NPU_USB_HID_or_UI_used':False}
(out/'result.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8');print(json.dumps(record,indent=2));assert passed
