"""Same-identity command diagnostics, not a gameplay accuracy measurement."""
import argparse
import csv
import hashlib
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('trace', type=Path)
p.add_argument('--output', required=True, type=Path)
args = p.parse_args()
with args.trace.open(encoding='utf-8-sig', newline='') as handle:
    rows = [{k: v if k == 'trace_id' else float(v) for k, v in row.items()}
            for row in csv.DictReader(handle)]

def active(row):
    return bool(int(row['flags']) & 1) and not int(row['flags']) & (2 | 16)

def consecutive(a, b):
    return (active(a) and active(b) and a['trace_id'] == b['trace_id']
            and b['id'] == a['id'] + 1 and a['lock_id'] == b['lock_id']
            and a['track_id'] == b['track_id']
            and 0 < b['observed_us'] - a['observed_us'] <= 30000)

def quantiles(values):
    values = sorted(values)
    return {f'p{q}': values[round((len(values) - 1) * q / 100)]
            for q in (50, 95, 99)} if values else {}

pairs = [(a, b) for a, b in zip(rows, rows[1:]) if consecutive(a, b)]
report = {
    'trace': str(args.trace), 'trace_sha256': hashlib.sha256(args.trace.read_bytes()).hexdigest(),
    'frames': len(rows), 'active_frames': sum(active(r) for r in rows),
    'same_identity_adjacent_pairs': len(pairs), 'axes': {},
    'scope': 'Same recorded lock and track, consecutive trace IDs, observation gap <=30ms. '
             'Commands opposite the current position error can be correct for a moving target; '
             'these counts are diagnostic, not physical jitter or wrong-move ground truth. '
             'Different live intervals are not controlled before/after gameplay comparisons.'}
for axis, bit in [('x', 512), ('y', 1024)]:
    delta = 'd' + axis
    flips = [(a, b) for a, b in pairs if a[delta] * b[delta] < 0
             and min(abs(a[delta]), abs(b[delta])) >= 2]
    same_side = [(a, b) for a, b in flips
                 if (a['target_' + axis] - 208) * (b['target_' + axis] - 208) > 0]
    output = {
        'strong_adjacent_reversals': len(flips),
        'reversals_with_position_still_on_same_side': len(same_side),
        'reversals_per_1000_same_identity_pairs': 1000 * len(flips) / len(pairs) if pairs else None,
        'output_delta_change_counts': quantiles([abs(a[delta] - b[delta]) for a, b in pairs]),
        'velocity_proposal_constrained_frames': sum(active(r) and bool(int(r['flags']) & bit) for r in rows),
        'examples': []}
    for a, b in sorted(same_side, key=lambda pair: abs(pair[0][delta]) + abs(pair[1][delta]), reverse=True)[:8]:
        keys = ['id', 'lock_id', 'track_id', 'target_' + axis, delta, 'velocity_' + axis,
                'predicted_error_' + axis, 'pending_' + axis, 'flags']
        output['examples'].append({'previous': {k: a[k] for k in keys},
                                   'next': {k: b[k] for k in keys}})
    report['axes'][axis] = output
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in report.items() if k != 'axes'} | {
    'axes': {axis: {k: v for k, v in value.items() if k != 'examples'}
             for axis, value in report['axes'].items()}}, ensure_ascii=False, indent=2))
