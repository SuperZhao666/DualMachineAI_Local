"""Export per-observation control telemetry; planned output is not physical ACK."""
import argparse, csv, json, math, re
from collections import Counter
from pathlib import Path
from control_trace_flags import ControlFlagInterpreter, LAYOUT_CHOICES

p = argparse.ArgumentParser()
p.add_argument('logs', nargs='+', type=Path)
p.add_argument('--process', required=True, type=int)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--control-flag-layout', choices=LAYOUT_CHOICES, default='auto',
               help='Explicit interpretation override; auto requires this process/trace manifest.')
args = p.parse_args()
fields = ['id', 'sequence', 'observed_us', 'processed_us', 'lock_id', 'track_id',
          'target_x', 'target_y', 'dx', 'dy', 'reason', 'flags', 'background_x', 'background_y',
          'background_support', 'pending_x', 'pending_y', 'pending_reports', 'accepted_at_us', 'accepted_dx', 'accepted_dy',
          'velocity_x', 'velocity_y', 'predicted_error_x', 'predicted_error_y', 'response_x', 'response_y', 'response_delay_us',
          'wire_frame_id', 'box_x1', 'box_y1', 'box_x2', 'box_y2', 'confidence']
integer_fields = set(fields) - {'target_x', 'target_y', 'background_x', 'background_y', 'pending_x', 'pending_y',
                             'velocity_x', 'velocity_y', 'predicted_error_x', 'predicted_error_y', 'response_x', 'response_y',
                             'box_x1', 'box_y1', 'box_x2', 'box_y2', 'confidence'}
schema = json.loads(Path(__file__).with_name('control-trace-schema.json').read_text(encoding='utf-8'))
flag_interpreter = ControlFlagInterpreter(schema, args.process, args.control_flag_layout)
fields.extend(schema['extra_fields'])
integer_fields.update(schema['extra_integer_fields'])
rows, estimates, pipeline = {}, [], []
for path in args.logs:
    for line in path.open(encoding='utf-8-sig'):
        try: event = json.loads(line)
        except ValueError: continue
        if event.get('process_id') != args.process: continue
        flag_interpreter.consume(event)
        detail = event.get('detail', '')
        match = re.search(r'control_trace_v([23456])=\[([^]]*)', detail)
        for encoded in match.group(2).split(';') if match else []:
            if not encoded: continue
            values = encoded.split(',')
            if len(values) == 18: values.extend(['0', '0', '0'])
            if len(values) == 21: values.extend(['0'] * 7)
            if len(values) == 28: values.extend(['0'] * 6)
            if int(match.group(1)) < 6 and len(values) == 34: values.extend(['0'] * len(schema['extra_fields']))
            if len(values) != len(fields): continue  # Legacy 32 kB logger may truncate the final row.
            try:
                row = {name: (int(value) if name in integer_fields else float(value)) for name, value in zip(fields, values)}
            except ValueError: continue
            row['trace_id'] = event['trace_id']
            row['schema_version'] = int(match.group(1))
            for key in ('model_width','model_height','capture_width','capture_height','coordinate_width','coordinate_height'):
                dimension=re.search(r'\b'+key+r'=(\d+)\b',detail)
                row[key]=int(dimension.group(1)) if dimension else row.get(key.replace('coordinate','model'),416)
            rows[(row['trace_id'], row['id'])] = row
        # Dense per-frame batches contain no aggregate estimator metrics. Avoid
        # scanning each 24 kB numeric batch once for every possible metric.
        if 'response_x_estimate=' not in detail:
            continue
        estimate = {'timestamp_utc': event['timestamp_utc']}
        for key in ['response_x_estimate', 'response_y_estimate', 'response_x_trusted', 'response_y_trusted',
                    'response_x_fit_quality', 'response_y_fit_quality', 'response_visibility_estimate_us',
                    'response_estimate_updates', 'background_motion_frames', 'background_motion_unavailable_frames',
                    'tracker_association_shift_frames', 'tracker_association_shift_rejections',
                    'tracker_known_owner_conflicts_resolved', 'tracker_lost_track_claims_blocked',
                    'tracker_global_assignment_calls', 'tracker_global_assignment_matches',
                    'tracker_global_assignment_size_fallbacks',
                    'tracker_joint_assignment_acceptances', 'tracker_joint_assignment_rejections',
                    'tracker_mixed_confidence_joint_frames', 'tracker_mixed_confidence_joint_rejections',
                    'retinal_velocity_constraints_x', 'retinal_velocity_constraints_y',
                    'pursuit_boosted_x_frames', 'pursuit_boosted_y_frames',
                    'body_shape_motion_resets',
                    'confirmed_stop_velocity_resets_x', 'confirmed_stop_velocity_resets_y',
                    'tracker_control_extent_confirmations',
                    'pending_motion_budget_holds', 'native_move_completion_timeouts', 'native_last_bluetooth_hid_api_acceptance_us']:
            m = re.search(r'\b' + key + r'=([\d.eE+-]+)', detail)
            if m: estimate[key] = float(m[1])
        for key in ['background_motion', 'receive_to_control', 'host_plus_receive_to_result', 'control_dispatch', 'qnn']:
            m = re.search(r'\b' + key + r'\{([^}]*)\}', detail)
            if m: estimate[key] = m[1]
        if 'response_x_estimate' in estimate: estimates.append(estimate)
ordered = sorted(rows.values(), key=lambda r: (r['trace_id'], r['id']))
# Rotated files are supplied newest-first by collectors. Their input order is
# not chronology; otherwise the "recent" fit can show an obsolete cold start.
estimates.sort(key=lambda estimate: estimate['timestamp_utc'])
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open('w', newline='', encoding='utf-8') as handle:
    writer = csv.DictWriter(handle, fields + ['trace_id', 'schema_version','model_width','model_height','capture_width','capture_height','coordinate_width','coordinate_height'])
    writer.writeheader(); writer.writerows(ordered)
def quantiles(values):
    values = sorted(values)
    return {f'p{q}': values[round((len(values)-1)*q/100)] for q in [50, 95, 99]} if values else {}
targets = [r for r in ordered if r['flags'] & 1]
active = [r for r in targets if not r['flags'] & (2 | 16)]
moves = [r for r in active if r['dx'] or r['dy']]
accepted = sorted({(r['trace_id'], r['accepted_at_us']) for r in ordered if r['accepted_at_us']})
flag_summary = flag_interpreter.summarize(ordered)
reason_names = ['none', 'invalid_time', 'stale_frame', 'non_monotonic_frame', 'no_valid_target',
                'detected_not_control_eligible', 'lock_held', 'switch_pending', 'deadzone', 'settle_guard',
                'response_guard', 'device_resolution_guard', 'direction_flip', 'motion_invalid']
report = {
    'process': args.process, 'frames': len(ordered), 'target_frames': len(targets), 'planned_move_frames': len(moves),
    'switches': sum(bool(r['flags'] & 4) for r in ordered),
    'background_valid_frames': sum(bool(r['flags'] & 8) for r in ordered),
    'association_shift_frames': sum(bool(r['flags'] & 128) for r in ordered),
    'known_owner_resolution_frames': sum(bool(r['flags'] & 256) for r in ordered),
    'retinal_velocity_constrained_x_frames': sum(bool(r['flags'] & 512) for r in ordered),
    'retinal_velocity_constrained_y_frames': sum(bool(r['flags'] & 1024) for r in ordered),
    'pursuit_boosted_x_frames': sum(bool(r['flags'] & 2048) for r in ordered),
    'pursuit_boosted_y_frames': sum(bool(r['flags'] & 4096) for r in ordered),
    'body_shape_motion_reset_frames': sum(bool(r['flags'] & 8192) for r in ordered),
    'projected_head_frames': flag_summary['head_source_frames']['projected'],
    'measured_head_frames': flag_summary['head_source_frames']['measured'],
    'large_background_motion_recovered_frames': flag_summary['large_background_motion_recovered_frames'],
    'configured_geometric_head_frames': flag_summary['configured_geometric_head_frames'],
    'control_flag_semantics': flag_summary,
    'tracking_only_frames': sum(bool(r['flags'] & 16) for r in ordered),
    'api_accepted_reports': len(accepted),
    'api_accepted_interval_us': quantiles([b[1]-a[1] for a,b in zip(accepted, accepted[1:]) if a[0]==b[0] and b[1]-a[1]<200000]),
    'trace_missing_ids': sum(max(0, b['id']-a['id']-1) for a,b in zip(ordered, ordered[1:]) if a['trace_id']==b['trace_id']),
    'retained_trace_ranges': [{'trace_id':trace, 'first_id':min(r['id'] for r in ordered if r['trace_id']==trace),
                              'last_id':max(r['id'] for r in ordered if r['trace_id']==trace)}
                             for trace in sorted(set(r['trace_id'] for r in ordered))],
    'observation_interval_us': quantiles([b['observed_us']-a['observed_us'] for a,b in zip(ordered, ordered[1:])
                                        if a['trace_id']==b['trace_id'] and b['id']==a['id']+1]),
    'planned_move_interval_us': quantiles([b['processed_us']-a['processed_us'] for a,b in zip(moves, moves[1:])
                                         if a['trace_id']==b['trace_id'] and b['processed_us']-a['processed_us']<200000]),
    'observation_age_us': quantiles([r['processed_us']-r['observed_us'] for r in ordered]),
    'target_error_pixels': quantiles([math.hypot(r['target_x']-r['coordinate_width']/2,r['target_y']-r['coordinate_height']/2) for r in active]),
    'target_error_screen_pixels':quantiles([math.hypot((r['target_x']-r['coordinate_width']/2)*r['capture_width']/r['coordinate_width'],(r['target_y']-r['coordinate_height']/2)*r['capture_height']/r['coordinate_height']) for r in active]),
    'suppression_reasons': dict(Counter(reason_names[r['reason']] if r['reason']<len(reason_names) else str(r['reason']) for r in ordered)),
    'recent_estimates': estimates[-12:],
    'semantics': 'Output is planned relative movement, not physical execution. Target error uses each trace control-coordinate dimensions; screen error also applies the reported capture scale. Current control coordinates are physical screen-ROI pixels while neural tensors remain 640-space. Legacy rows without dimensions assume the former 416/416 contract. Errors include user actions and detection geometry. Observation age excludes unknown one-way network delay.'}
args.output.with_suffix('.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
