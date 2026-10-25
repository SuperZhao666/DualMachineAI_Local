"""Extract bounded native response telemetry without claiming physical HID ACK."""
import argparse, csv, json, re
from pathlib import Path
from collections import Counter

parser = argparse.ArgumentParser()
parser.add_argument('logs', nargs='+', type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--process', type=int, help='Only this phone process; avoid mixing installed builds')
args = parser.parse_args()
fields = ['id', 'observed_at_us', 'dx', 'dy', 'raw_x', 'raw_y', 'resolved_x',
          'resolved_y', 'error_x', 'error_y', 'support', 'wait_us', 'flags']
samples = {}
for path in args.logs:
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if args.process is not None and event.get('process_id') != args.process:
            continue
        match = re.search(r'visible_response_trace_v1=\[([^]]*)\]', event.get('detail', ''))
        if not match:
            continue
        for item in match.group(1).split(';'):
            if not item:
                continue
            row = dict(zip(fields, map(float, item.split(','))))
            for name in ['id', 'observed_at_us', 'dx', 'dy', 'support', 'wait_us', 'flags']:
                row[name] = int(row[name])
            row['process_id'] = event['process_id']
            row['trace_id'] = event['trace_id']
            row['log_timestamp_utc'] = event['timestamp_utc']
            samples[(row['trace_id'], row['id'])] = row
rows = sorted(samples.values(), key=lambda r: (r['trace_id'], r['id']))
args.output.parent.mkdir(parents=True, exist_ok=True)
with args.output.open('w', newline='', encoding='utf-8') as handle:
    writer = csv.DictWriter(handle, fieldnames=fields + ['process_id', 'trace_id', 'log_timestamp_utc'])
    writer.writeheader()
    writer.writerows(rows)
timeouts = [r for r in rows if r['flags'] & 8]
def quantiles(values):
    values = sorted(values)
    return {f'p{p}': values[min(len(values)-1, int((len(values)-1)*p/100))] for p in [50, 95, 99]} if values else {}
report = {'samples': len(rows), 'timeouts': len(timeouts),
          'timeout_flags': dict(Counter(r['flags'] for r in timeouts)),
          'wait_us': quantiles([r['wait_us'] for r in rows]),
          'successful_response_interval_us': quantiles([b['observed_at_us'] - a['observed_at_us']
              for a, b in zip(rows, rows[1:]) if a['trace_id'] == b['trace_id'] and b['id'] == a['id'] + 1
              and not (a['flags'] & 8) and not (b['flags'] & 8)]),
          'semantics': 'Visual correspondence after Android HID API acceptance; not physical execution timestamp',
          'output': str(args.output)}
args.output.with_suffix('.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
print('Last failed responses:')
for row in timeouts[-12:]:
    print(json.dumps({k: row[k] for k in fields}))
