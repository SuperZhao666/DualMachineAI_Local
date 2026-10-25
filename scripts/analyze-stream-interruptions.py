"""Summarize VF receive/inference stalls from retained, process-bound diagnostics.

Five-second health samples are not packet captures. A fresh-observation gap can
also mean an unchanged picture; report it separately from aged complete frames.
"""
import argparse
import datetime
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('collections', nargs='+', type=Path)
parser.add_argument('--process', required=True, type=int)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
events = {}
for collection in args.collections:
    for path in collection.glob('visionforge-mobile-runtime.jsonl*'):
        for line in path.read_text(encoding='utf-8-sig', errors='replace').splitlines():
            try:
                row = json.loads(line)
            except ValueError:
                continue
            if row.get('process_id') == args.process:
                events[(row['trace_id'], row['sequence'])] = row
ordered = sorted(events.values(), key=lambda row: row['timestamp_unix_ms'])
if not ordered:
    raise SystemExit('No events for the requested process')

def value(detail, key):
    match = re.search(r'(?<![\w_])' + re.escape(key) + r'=(-?\d+(?:\.\d+)?)', detail)
    return float(match[1]) if match and '.' in match[1] else int(match[1]) if match else None

def utc(ms):
    return datetime.datetime.fromtimestamp(ms / 1000, datetime.timezone.utc).isoformat()

keys = ['accepted_datagrams', 'reassembled_access_units', 'decoder_accepted_access_units',
        'idr_requests', 'receiver_timeouts', 'reassembly_expirations',
        'reassembly_incomplete_losses', 'reassembly_dependent_flushes',
        'reassembly_idr_skipped_access_units', 'reference_resync_drops',
        'reference_resync_dependent_flushes', 'last_reassembled_access_unit_age_ms',
        'qnn_executions', 'qnn_failures', 'source_changes', 'source_restart_failures',
        'fatal_socket_errors', 'response_x_estimate', 'response_y_estimate']
timeline, observations, metrics, transitions = [], {}, [], []
for event in ordered:
    detail = event.get('detail', '')
    if event['event'] == 'runtime_pipeline_detail':
        timeline.append({'timestamp_utc': event['timestamp_utc'],
                         'timestamp_unix_ms': event['timestamp_unix_ms'],
                         'elapsed_ms': event['elapsed_ms'],
                         **{key: value(detail, key) for key in keys}})
    if event['event'] in ['mobile_pipeline_metrics', 'runtime_pipeline_metrics']:
        metrics.append({'timestamp_utc': event['timestamp_utc'],
                        'elapsed_ms': event['elapsed_ms'],
                        **{key: value(detail, key) for key in [
                            'qnn_fps', 'wire_reassembly_fps', 'qnn_failures',
                            'preprocess_p50', 'qnn_p50', 'decode_queue_p50', 'thermal_status']}})
    if event['event'] in ['control_output_auto_locked', 'control_output_auto_ready',
                          'local_pipeline_started', 'local_pipeline_stopped',
                          'control_calibration_restored', 'makcu_protocol_negotiation_completed']:
        transitions.append({key: event[key] for key in
                            ['timestamp_utc', 'elapsed_ms', 'event', 'detail']})
    match = re.search(r'control_trace_v5=\[([^]]*)', detail)
    for encoded in match[1].split(';') if match else []:
        parts = encoded.split(',')
        if len(parts) != 34:
            continue
        try:
            observations[(event['trace_id'], int(parts[0]))] = (
                int(parts[2]), int(parts[3]), int(parts[28]))
        except ValueError:
            pass

stalls = []
for index, row in enumerate(timeline):
    age = row['last_reassembled_access_unit_age_ms']
    if age is None or age < 2000:
        continue
    inferred_last = row['timestamp_unix_ms'] - age
    if not stalls or abs(stalls[-1]['estimated_last_complete_frame_unix_ms'] - inferred_last) > 100:
        previous = timeline[max(0, index - 1)]
        stalls.append({'estimated_last_complete_frame_unix_ms': inferred_last,
                       'estimated_last_complete_frame_utc': utc(inferred_last),
                       'maximum_observed_frame_age_ms': age,
                       'packets_at_previous_sample': previous['accepted_datagrams'],
                       'packets_at_last_stalled_sample': row['accepted_datagrams'],
                       'first_stalled_sample_utc': row['timestamp_utc'],
                       'last_stalled_sample_utc': row['timestamp_utc']})
    else:
        stalls[-1]['maximum_observed_frame_age_ms'] = max(stalls[-1]['maximum_observed_frame_age_ms'], age)
        stalls[-1]['packets_at_last_stalled_sample'] = row['accepted_datagrams']
        stalls[-1]['last_stalled_sample_utc'] = row['timestamp_utc']

gaps, missing_ids = [], 0
observations = sorted(observations.items())
for (left_key, left), (right_key, right) in zip(observations, observations[1:]):
    if left_key[0] != right_key[0]:
        continue
    missing_ids += max(0, right_key[1] - left_key[1] - 1)
    if right_key[1] == left_key[1] + 1 and right[1] - left[1] >= 100_000:
        gaps.append({'after_trace_id': left_key[1], 'processed_gap_ms': (right[1] - left[1]) / 1000,
                     'wire_sequence_before': left[2], 'wire_sequence_after': right[2]})
active = [row for row in timeline if row['elapsed_ms'] >= 15_000]
fps = [row['qnn_fps'] for row in metrics if row['elapsed_ms'] >= 15_000 and row['qnn_fps'] is not None]
post_startup_locks = [event for event in transitions if event['elapsed_ms'] >= 15_000
                      and event['event'] == 'control_output_auto_locked']
report = {
    'process_id': args.process, 'collections': [str(path) for path in args.collections],
    'retained_start_utc': ordered[0]['timestamp_utc'], 'retained_end_utc': ordered[-1]['timestamp_utc'],
    'retained_seconds': (ordered[-1]['timestamp_unix_ms'] - ordered[0]['timestamp_unix_ms']) / 1000,
    'health_sample_count': len(timeline), 'long_complete_frame_stalls': stalls,
    'post_startup_auto_locks': post_startup_locks,
    'maximum_sampled_complete_frame_age_ms': max((row['last_reassembled_access_unit_age_ms'] or 0 for row in active), default=None),
    'qnn_fps_sample_range': [min(fps), max(fps)] if fps else [],
    'trace_observations': len(observations), 'missing_trace_ids': missing_ids,
    'fresh_observation_gaps_over_100ms': sorted(gaps, key=lambda row: row['processed_gap_ms'], reverse=True)[:30],
    'first_health': timeline[0] if timeline else None, 'last_health': timeline[-1] if timeline else None,
    'transitions': transitions, 'timeline': timeline, 'metrics': metrics,
    'limitations': ['Health samples are periodic, not exact per-packet recovery timing',
                    'Unchanged images legitimately reduce fresh QNN observations',
                    'No external measurement of physical end-to-end latency or packet loss origin']}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps({key: report[key] for key in [
    'process_id', 'retained_seconds', 'health_sample_count', 'long_complete_frame_stalls',
    'post_startup_auto_locks', 'maximum_sampled_complete_frame_age_ms',
    'qnn_fps_sample_range', 'trace_observations', 'missing_trace_ids', 'last_health']}, indent=2))
