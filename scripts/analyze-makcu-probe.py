"""Summarize the bounded movement probe without claiming a synchronized physical ACK."""
import csv
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
folder = root / 'audit/makcu-20261018'
raw = [{key: int(value) for key, value in row.items()}
       for row in csv.DictReader((folder / 'physical-pattern-raw.csv').open())]
phone = json.loads((folder / 'physical-pattern-phone.json').read_text())
clock = json.loads((folder / 'clock-offset-persistent.json').read_text())
matches = []
used = set()
# A broad search first established a cluster around PC time - phone time = 50ms.
# That includes clock skew: it is NOT a measured 50ms physical execution delay.
for command in phone['commands']:
    candidates = [(i, row) for i, row in enumerate(raw)
                  if i not in used and row['dx'] == command['dx'] and row['dy'] == command['dy']
                  and 45000 <= row['unix_us'] - command['sent_unix_ms'] * 1000 <= 55000]
    if len(candidates) == 1:
        i, row = candidates[0]
        used.add(i)
        matches.append({'command_index': command['index'], 'raw_index': i,
                        'dx': command['dx'], 'dy': command['dy'],
                        'pc_minus_phone_timestamp_us': row['unix_us'] - command['sent_unix_ms'] * 1000})
report = {'scope': 'pattern correspondence in mixed manual/device Raw Input; not isolated latency measurement',
          'commands': len(phone['commands']), 'echo_successful': phone['successful'],
          'raw_samples': len(raw), 'raw_devices': len({r['device_id'] for r in raw}),
          'unambiguous_exact_pattern_matches': len(matches), 'matches': matches,
          'net_command_x': sum(c['dx'] for c in phone['commands']),
          'maximum_single_command': max(abs(c['dx']) for c in phone['commands']),
          'clock_bounds_after_test': {k: v for k, v in clock.items() if k != 'samples'},
          'physical_latency_verified': False,
          'limitation': 'Unmatched commands may be merged with manual input; do not count them as lost. Clock bounds were measured later, and no game-frame response was measured.',
          'inputs': {name: hashlib.sha256((folder / name).read_bytes()).hexdigest()
                     for name in ['physical-pattern-raw.csv', 'physical-pattern-phone.json', 'clock-offset-persistent.json']}}
(folder / 'physical-pattern-analysis.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in report.items() if k not in {'matches', 'inputs', 'clock_bounds_after_test'}}, indent=2))
