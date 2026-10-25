"""Drive deterministic on-device video replay without desktop window focus."""
from pathlib import Path
import argparse, json, subprocess, time, socket, threading, uuid

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
def physical_capture_dimension(value):
    parsed = int(value)
    if not 64 <= parsed <= 4096:
        raise argparse.ArgumentTypeError('physical capture dimensions must be 64..4096 pixels')
    return parsed
parser.add_argument('--device', required=True)
parser.add_argument('--phone-ip', required=True)
parser.add_argument('--host-ip', required=True)
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--capture-width', type=physical_capture_dimension, required=True,
    help='Original physical recording crop width; do not infer it from encoded video size')
parser.add_argument('--capture-height', type=physical_capture_dimension, required=True,
    help='Original physical recording crop height; current captures use 640, legacy 1K captures use 1024')
parser.add_argument('--frames', type=int, required=True)
parser.add_argument('--soak-repeats', type=int, default=8)
parser.add_argument('--fps',type=int,default=30)
parser.add_argument('--saturation',action='store_true',help='Measure overload behavior; does not assert 90 percent inference completeness')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--host-already-stopped', action='store_true', help='Confirm exclusive video receiver ownership after stopping the desktop Host')
parser.add_argument('--headless', action='store_true', help='Use isolated test process, not Activity/service lifecycle')
parser.add_argument('--device-local', action='store_true', help='Isolate production pipeline from Wi-Fi using a test-only local UDP source; requires --headless')
parser.add_argument('--foreground-activity', action='store_true', help='Keep the real VF Activity visible for foreground Wi-Fi performance while the isolated test owns decode')
parser.add_argument('--projection-probe', action='store_true', help='Test-only fake sink in isolated benchmark process; cannot touch USB/HID or launch MainActivity')
parser.add_argument('--projection-expected-mode', type=int, choices=(0,1,2), default=2, help='Expected property mode for the fake-sink comparison, default strict active mode2')
args = parser.parse_args()
if args.device_local and not args.headless: parser.error('--device-local requires --headless')
if args.foreground_activity and not args.headless: parser.error('--foreground-activity requires --headless')
if args.projection_probe and (not args.headless or args.foreground_activity): parser.error('--projection-probe requires --headless and excludes --foreground-activity')
args.output.mkdir(parents=True, exist_ok=True)
adb = ['adb', '-s', args.device]
remote_directory='video-headless' if args.headless else 'video-replay'
runner='OverwatchBenchmark'
command = adb + ['shell', 'am', 'instrument', '-w', '-e', 'mode', 'headless_video' if args.headless else 'video_replay',
    '-e', 'phone_ip', args.phone_ip, '-e', 'host_ip', args.host_ip,
    '-e', 'capture_width', str(args.capture_width), '-e', 'capture_height', str(args.capture_height),
    '-e', 'frames', str(args.frames), '-e', 'soak_repeats', str(args.soak_repeats),
    'com.visionforge.mobile.local.test/com.visionforge.inferencebenchmark.'+runner]
run_id=uuid.uuid4().hex
command[-1:-1]=['-e','run_id',run_id]
command[-1:-1]=['-e','fps',str(args.fps),'-e','saturation',str(args.saturation).lower()]
if args.foreground_activity: command[-1:-1]=['-e','foreground_activity','true']
if args.projection_probe: command[-1:-1]=['-e','projection_probe','true','-e','projection_expected_mode',str(args.projection_expected_mode)]
if args.foreground_activity:
    # Android may silently deny an Activity launch from background instrumentation.
    # Launch through the explicitly authorized ADB test session first.
    subprocess.run(adb+['shell','am','start','-W','-n',
        'com.visionforge.mobile.local/com.visionforge.inferencebenchmark.MainActivity'],check=True)
if args.device_local:
    fixture='/sdcard/Android/data/com.visionforge.mobile.local/files/video-fixture.h264'
    subprocess.run(adb+['push',str(args.input.resolve()),fixture],check=True)
    command[-1:-1]=['-e','local_replay',fixture]
replays = []
probe_stop = threading.Event()
probe_acks = []
def discover_receiver():
    # Exercise the production local UDP discovery protocol even when no desktop
    # capture window is running. This emits no mouse/button or video packets.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind((args.host_ip, 0)); udp.settimeout(.2)
        token = 1
        while not probe_stop.is_set():
            message = ('VF_CAT6_PROBE_V1|'+str(token)+'|').encode().ljust(1200,b'\0')
            try:
                udp.sendto(message, (args.phone_ip, 5004))
                data, peer = udp.recvfrom(2048)
                if peer[0] == args.phone_ip and data.startswith(('VF_CAT6_PROBE_ACK_V1|'+str(token)+'|').encode()):
                    probe_acks.append(token)
            except (TimeoutError, OSError): pass
            token += 1
            probe_stop.wait(.25)
probe = threading.Thread(target=discover_receiver, daemon=True)
if not args.headless: probe.start()
process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    encoding='utf-8', errors='replace', bufsize=1)
try:
    with (args.output/'instrumentation.log').open('w', encoding='utf-8') as log:
        for line in process.stdout:
            log.write(line); log.flush()
            print(line.rstrip(), flush=True)
            start = line.find('{')
            if start < 0: continue
            try: event = json.loads(line[start:])
            except json.JSONDecodeError: continue
            if event.get('launch_activity'):
                subprocess.run(adb + ['shell', 'am', 'start', '-W', '-n',
                    'com.visionforge.mobile.local/com.visionforge.inferencebenchmark.MainActivity'],
                    stdout=subprocess.DEVNULL, check=True)
            if event.get('pause_desktop_host') and args.host_already_stopped:
                subprocess.run(adb + ['shell', 'touch', '/sdcard/Android/data/com.visionforge.mobile.local/files/video-replay/host-paused'], check=True)
            if 'ready' not in event: continue
            if args.device_local: continue
            if replays and replays[-1][0].poll() is None:
                raise RuntimeError('Previous replay still running at model transition')
            destination = (args.output/(event['ready']+'-sender.log')).open('w', encoding='utf-8')
            replay = subprocess.Popen([str(root/'build/host/VisionForgeDatasetReplay.exe'),
                '--input', str(args.input.resolve()), '--host', args.phone_ip, '--port', '5000',
                '--capture-width', str(args.capture_width), '--capture-height', str(args.capture_height),
                '--source-port', '5101', '--fps', str(args.fps), '--repeat', str(event['repeat'])],
                stdout=destination, stderr=subprocess.STDOUT)
            replays.append((replay, destination))
    process.wait()
finally:
    probe_stop.set()
    if probe.is_alive(): probe.join(timeout=2)
    (args.output/'discovery.json').write_text(json.dumps({'acknowledged_probes':len(probe_acks),'host_ip':args.host_ip,'phone_ip':args.phone_ip}), encoding='utf-8')
    for replay, log in replays:
        if replay.poll() is None:
            replay.terminate(); replay.wait(timeout=10)
        log.close()
    if process.poll() is None: process.terminate()
    subprocess.run(adb + ['pull', '/sdcard/Android/data/com.visionforge.mobile.local/files/'+remote_directory,
        str(args.output)], check=True)
report = json.loads((args.output/remote_directory/'video-replay.json').read_text(encoding='utf-8'))
if args.headless and report.get('run_id')!=run_id: raise SystemExit('Stale or missing report: run_id mismatch')
if not report.get('passed'): raise SystemExit('Video replay failed; inspect saved report')
print('PASS all video replay phases', flush=True)
