"""Summarize bounded VF evidence without equating software receipts with motion.

The detection archive and live collection may be from different processes.
Their statistics are deliberately separate; neither labels physical identity.
"""
import argparse
import csv
import hashlib
import json
import math
import re
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--collection', type=Path, required=True)
p.add_argument('--process', type=int, required=True)
p.add_argument('--detections', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
metrics = []
keys = [
    'geometry_out_of_bounds_rejected', 'geometry_size_rejected',
    'geometry_aspect_rejected', 'rejected_exact_track_geometry',
    'rejected_known_track_rebinds', 'rejected_track_id_mismatches',
    'native_device_ack_completions', 'native_move_completion_timeouts',
    'java_offer_rejections', 'tracker_ambiguous_center_matches_rejected',
    'response_x_estimate', 'response_y_estimate', 'response_visibility_estimate_us',
    'offer_to_completion_us_p50', 'offer_to_completion_us_p95', 'offer_to_completion_us_p99',
]
files = {}
for path in sorted(a.collection.glob('visionforge-mobile-runtime.jsonl*')):
    files[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        try:
            e = json.loads(line)
        except ValueError:
            continue
        if e.get('process_id') != a.process:
            continue
        detail = e.get('detail', '')
        if 'geometry_out_of_bounds_rejected=' not in detail:
            continue
        row = {'timestamp': e['timestamp_utc']}
        for key in keys:
            match = re.search(r'\b' + key + r'=([\d.eE+-]+)', detail)
            if match:
                row[key] = float(match[1])
        metrics.append(row)
metrics.sort(key=lambda r: r['timestamp'])
assert metrics, 'No matching control telemetry'

det = {}
for path in sorted(a.detections.glob('detections-*.csv')):
    files[str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    for raw in csv.DictReader(path.open(encoding='utf-8-sig')):
        r = {k: float(v) for k, v in raw.items()}
        if r['class_id'] < 0:
            continue
        assert all(math.isfinite(v) for v in r.values())
        key = tuple(r[k] for k in ['frame_id', 'class_id', 'confidence', 'x1', 'y1', 'x2', 'y2'])
        det[key] = r
outside = []
recoverable = []
near_epsilon = []
for r in det.values():
    if not (r['x1'] < 0 or r['y1'] < 0 or r['x2'] > 416 or r['y2'] > 416):
        continue
    width, height = r['x2']-r['x1'], r['y2']-r['y1']
    vw = min(416, r['x2']) - max(0, r['x1'])
    vh = min(416, r['y2']) - max(0, r['y1'])
    r = dict(r, visible_fraction=max(0, vw)*max(0, vh)/(width*height))
    outside.append(r)
    overshoot = max(0, -r['x1'], -r['y1'], r['x2']-416, r['y2']-416)
    if overshoot <= 2:
        near_epsilon.append(r)
    if width <= 832 and height <= 832 and 2 <= vw <= 416 and 2 <= vh <= 416 and .02 <= vw/vh <= 50 and r['visible_fraction'] >= .25:
        recoverable.append(r)
report = {
    'scope': 'Source-linked telemetry and box geometry only; no ground-truth person identity or physical game execution claim.',
    'process': a.process,
    'live_collection': str(a.collection),
    'live_first': metrics[0],
    'live_last': metrics[-1],
    'archive_detection_directory': str(a.detections),
    'archive_unique_detections': len(det),
    'archive_out_of_bounds': len(outside),
    'archive_out_of_bounds_frames': len({r['frame_id'] for r in outside}),
    'archive_overshoot_at_most_2px': len(near_epsilon),
    'archive_partial_geometry_eligible_after_fix': len(recoverable),
    'archive_sample_partial_detections': sorted(recoverable, key=lambda r: -r['visible_fraction'])[:8],
    'files_sha256': files,
}
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in report.items() if k not in ['files_sha256', 'archive_sample_partial_detections']}, indent=2))
