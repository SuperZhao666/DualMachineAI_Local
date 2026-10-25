"""Measure per-frame pipeline stages, preserving clock and sampling limits.

Reads saved logs only. Marginal percentiles must never be added to construct a
total; every reported total here is computed from the same frame's timestamps.
"""
import argparse
import csv
import hashlib
import json
import re
from pathlib import Path

SCHEMA = Path(__file__).with_name('control-trace-schema.json')
FIELDS = json.loads(SCHEMA.read_text(encoding='utf-8'))['pipeline_fields']
TIMES = FIELDS[4:15]
STAGES = {
    'host_reported_age_us': ('observed_us', 'received_us'),
    'phone_receive_to_decoded_planes_us': ('received_us', 'decoded_planes_us'),
    'preprocess_entry_gap_us': ('decoded_planes_us', 'preprocess_started_us'),
    'preprocess_us': ('preprocess_started_us', 'preprocess_finished_us'),
    'preprocess_to_enqueue_us': ('preprocess_finished_us', 'enqueued_us'),
    'inference_queue_and_entry_us': ('enqueued_us', 'inference_started_us'),
    'inference_total_us': ('inference_started_us', 'inference_finished_us'),
    'auxiliary_total_us': ('inference_finished_us', 'result_ready_us'),
    'result_commit_to_control_entry_us': ('result_ready_us', 'control_started_us'),
    'control_dispatch_us': ('control_started_us', 'control_finished_us'),
    'phone_receive_to_control_us': ('received_us', 'control_finished_us'),
    'estimated_observation_to_control_us': ('observed_us', 'control_finished_us'),
}


def quantiles(values):
    values = sorted(values)
    if not values:
        return {'n': 0}
    return {'n': len(values), **{
        f'p{q}': values[round((len(values)-1)*q/100)] for q in (50, 95, 99)
    }, 'max': values[-1]}


def summarize(rows):
    result = {stage: quantiles([r[end]-r[start] for r in rows])
              for stage, (start, end) in STAGES.items()}
    for name in ('qnn_us', 'background_us', 'image_motion_us'):
        result[name] = quantiles([r[name] for r in rows])
    return result


def analyze(collection, process, control_csv=None):
    rows, files, events_seen = {}, {}, set()
    invalid, duplicate_rows, duplicate_events, lost = [], 0, 0, 0
    conflicts = set()
    for path in sorted(collection.glob('visionforge-mobile-runtime.jsonl*')):
        files[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        with path.open(encoding='utf-8-sig') as handle:
            for line_number, line in enumerate(handle, 1):
                try:
                    event = json.loads(line)
                except ValueError:
                    continue  # Interrupted final writes cannot form a measurement.
                if event.get('process_id') != process:
                    continue
                event_key = (event.get('trace_id'), event.get('sequence'))
                if event_key in events_seen:
                    duplicate_events += 1
                    continue
                events_seen.add(event_key)
                detail = event.get('detail', '')
                if event.get('event') == 'runtime_trace_batch':
                    m = re.search(r'\bpipeline_trace_lost=(\d+)', detail)
                    if m:
                        lost += int(m[1])
                match = re.search(r'\bpipeline_trace_v1=\[([^]]*)\]', detail)
                if not match:
                    continue
                for encoded in filter(None, match[1].split(';')):
                    values = encoded.split(',')
                    if len(values) != len(FIELDS) or any(not re.fullmatch(r'\d+', v) for v in values):
                        invalid.append({'file': path.name, 'line': line_number, 'reason': 'invalid_fields'})
                        continue
                    row = dict(zip(FIELDS, map(int, values)))
                    row['trace_id'] = event['trace_id']
                    key = (row['trace_id'], row['id'])
                    if key in rows:
                        duplicate_rows += 1
                        if rows[key] != row:
                            conflicts.add(key)
                        continue
                    rows[key] = row
    valid = []
    for key, row in sorted(rows.items()):
        reason = None
        if key in conflicts:
            reason = 'conflicting_duplicate'
        elif not row['id'] or not row['generation'] or row['content_updated'] not in (0, 1):
            reason = 'invalid_identity_or_flags'
        elif any(not row[t] for t in TIMES):
            reason = 'missing_timestamp'
        elif any(row[a] > row[b] for a, b in zip(TIMES, TIMES[1:])):
            reason = 'nonmonotonic_timestamps'
        # Duration measurements truncate sub-microseconds independently.
        elif row['qnn_us'] > row['inference_finished_us']-row['inference_started_us']+2:
            reason = 'qnn_exceeds_enclosing_inference'
        elif row['background_us']+row['image_motion_us'] > row['result_ready_us']-row['inference_finished_us']+2:
            reason = 'auxiliary_exceeds_enclosing_interval'
        if reason:
            invalid.append({'trace_id': row['trace_id'], 'id': row['id'], 'reason': reason})
        else:
            valid.append(row)

    control = None
    if control_csv is not None:
        controls = {}
        with control_csv.open(encoding='utf-8-sig', newline='') as handle:
            for c in csv.DictReader(handle):
                key = (c['trace_id'], int(c['sequence']), int(c['wire_frame_id']), int(c['observed_us']))
                controls[key] = c
        joined, active, unmatched = 0, [], 0
        for row in valid:
            key = (row['trace_id'], row['sequence'], row['wire_frame_id'], row['observed_us'])
            c = controls.get(key)
            if c is None:
                unmatched += 1
                continue
            joined += 1
            if int(c['flags']) & (1 | 2 | 16) == 1:
                active.append(row)
        control = {'source': str(control_csv), 'sha256': hashlib.sha256(control_csv.read_bytes()).hexdigest(),
                   'joined_frames': joined, 'unmatched_frames': unmatched,
                   'active_target_frames': len(active), 'active_target_stages': summarize(active)}
    return {
        'process': process, 'pipeline_schema': 1, 'unique_rows': len(rows), 'valid_rows': len(valid),
        'reported_lost_rows': lost, 'duplicate_events': duplicate_events, 'duplicate_rows': duplicate_rows,
        'invalid_rows': invalid, 'stages': summarize(valid), 'control_join': control,
        'semantics': {
            'sample': 'Successful current-generation inference frames only; superseded/skipped/failing frames are absent.',
            'clock': 'Phone monotonic microseconds; estimated observation excludes unknown one-way network latency.',
            'decode': 'Receive to native decoded-plane callback includes codec queues, scheduling and callback/lock delay.',
            'queue': 'Inference enqueue to inference entry includes worker wake and generation-check lock delay.',
            'nested': 'qnn_us is inside inference_total; background_us and image_motion_us are inside auxiliary_total.',
            'totals': 'Computed per frame; do not sum percentile columns. No physical mouse or game hit-rate claim.',
        },
        'files_sha256': files,
        'schema_sha256': hashlib.sha256(SCHEMA.read_bytes()).hexdigest(),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('collection', type=Path)
    p.add_argument('--process', type=int, required=True)
    p.add_argument('--control-csv', type=Path)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    report = analyze(args.collection, args.process, args.control_csv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('process', 'unique_rows', 'valid_rows', 'reported_lost_rows', 'invalid_rows', 'stages')}, indent=2))


if __name__ == '__main__':
    main()
