"""Bounded game/model evidence using the existing local diagnostic intent."""
import argparse, json, subprocess, time
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--device', required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--bursts', type=int, default=10)
a = p.parse_args()
if not 1 <= a.bursts <= 12: p.error('bursts must be 1-12')
a.output.mkdir(parents=True, exist_ok=True)
adb = ['adb', '-s', a.device]
intent = adb + ['shell', 'am', 'start', '-n',
    'com.visionforge.mobile.local/com.visionforge.inferencebenchmark.MainActivity',
    '--es', 'com.visionforge.mobile.diagnostic.action']
def run(command):
    return subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15)
reports = []
for i in range(a.bursts):
    started = time.time()
    begin = run(intent + ['begin_detection_trace', '--ei', 'com.visionforge.mobile.diagnostic.trace_frame_count', '500'])
    time.sleep(4.6)
    export = run(intent + ['export_detection_trace'])
    pull = run(adb + ['pull', '/sdcard/Android/data/com.visionforge.mobile.local/files/diagnostics/qnn-detection-trace.csv',
        str(a.output / f'detections-{i:02d}.csv')])
    report = {'burst': i, 'start_unix': started, 'end_unix': time.time(),
              'begin_status': begin.returncode, 'export_status': export.returncode, 'pull_status': pull.returncode}
    reports.append(report)
    print(json.dumps(report), flush=True)
    (a.output / 'bursts.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')
