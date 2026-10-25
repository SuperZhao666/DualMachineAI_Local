"""Identity- and coverage-bounded offline API/background correlation."""
import argparse
import csv
import json
import math
import hashlib
import io
from collections import Counter, OrderedDict
from pathlib import Path
import numpy as np
from control_trace_geometry import center


def integer(row, key):
    value = float(row[key])
    if not math.isfinite(value) or value != int(value):
        raise ValueError(f'invalid integer {key}')
    return int(value)


class Context:
    def __init__(self, identity, rows):
        self.identity, self.rows = identity, rows
        self.at = np.array([integer(r, 'observed_us') for r in rows], dtype=np.int64)
        self.ids = np.array([integer(r, 'id') for r in rows], dtype=np.int64)
        self.centers = np.array([[center(r, 'x'), center(r, 'y')] for r in rows])
        self.a = {k: np.array([float(r[k]) for r in rows]) for k in
                  ('flags', 'background_x', 'background_y', 'target_x', 'target_y', 'lock_id')}
        if any(not np.isfinite(v).all() for v in self.a.values()):
            raise ValueError('nonfinite trace value')
        if np.any(np.diff(self.at) <= 0) or np.any(np.diff(self.ids) <= 0):
            raise ValueError('duplicate or nonmonotonic control id/time within one trace; no implicit sorting')
        self.missing = int(np.maximum(0, np.diff(self.ids)-1).sum())
        self.processed = (np.array([integer(r, 'processed_us') for r in rows], dtype=np.int64)
                          if all('processed_us' in r for r in rows) else None)
        commands = {}
        for row in rows:
            at = integer(row, 'accepted_at_us')
            if at < 0: raise ValueError('negative API acceptance time')
            if not at: continue
            delta = integer(row, 'accepted_dx'), integer(row, 'accepted_dy')
            if at in commands and commands[at] != delta:
                raise ValueError('conflicting accepted command in one trace')
            commands[at] = delta
        self.cmd_at = np.array(sorted(commands), dtype=np.int64)
        xy = np.array([commands[t] for t in self.cmd_at], dtype=float).reshape(-1, 2)
        self.cmd_sum = np.vstack([np.zeros(2), xy.cumsum(axis=0)])
        self.bg_sum = np.vstack([np.zeros(2), np.column_stack(
            [self.a['background_x'], self.a['background_y']]).cumsum(axis=0)])
        self.edge = np.zeros(len(rows), dtype=bool)
        self.reasons, self.segments = Counter(), []
        begin = 0
        for i in range(1, len(rows)):
            reason = None
            if self.ids[i] != self.ids[i-1]+1: reason = 'missing_control_id'
            elif self.at[i]-self.at[i-1] >= 20_000: reason = 'fit_cadence_outside_20ms_contract'
            elif not int(self.a['flags'][i]) & 8: reason = 'background_invalid'
            elif self.processed is not None and self.processed[i] <= self.processed[i-1]:
                reason = 'processed_time_discontinuity'
            else:
                for key in ('background_previous_us', 'background_previous_observed_us',
                            'background_previous_observed_at_us'):
                    if key in rows[i] and integer(rows[i], key) != self.at[i-1]:
                        reason = 'background_previous_mismatch'
                        break
            if reason:
                self.reasons[reason] += 1
                self.segments.append((begin, i)); begin = i
            else: self.edge[i] = True
        self.segments.append((begin, len(rows)))
        self.rejected_command_bins = set()
        self.structural_bins = self.structural_rejected = 0
        for span in (2, 4, 8):
            for last in range(span, len(rows), span):
                self.structural_bins += 1
                if not self.edge[last-span+1:last+1].all(): self.structural_rejected += 1

    def binding(self, segment=None):
        result = dict(zip(('process_id', 'trace_id', 'stream_generation'), self.identity))
        if segment is not None: result['segment'] = segment
        return result

    def fit(self, begin, end, axis):
        if not len(self.cmd_at) or self.processed is None: return []
        fits = []
        for segment, (left, right) in enumerate(self.segments):
            for span in (2, 4, 8):
                last = np.arange(max(left, begin)+span, min(right, end), span)
                first = last-span
                if len(last) < 10: continue
                seconds = (self.at[last]-self.at[first])/1e6
                original_y = (self.bg_sum[last+1, axis]-self.bg_sum[first+1, axis])/seconds
                for delay in range(0, 120001, 2000):
                    low, high = self.at[first]-delay, self.at[last]-delay
                    # A complete background segment does not prove that earlier
                    # accepted commands across a log hole were also retained.
                    ok = (low >= self.processed[left]) & (high <= self.processed[right-1])
                    for f, l in zip(first[~ok], last[~ok]):
                        self.rejected_command_bins.add((int(f), int(l), span, delay))
                    if np.count_nonzero(ok) < 10: continue
                    y = original_y[ok]; y = y-y.mean(); yy = y@y
                    if yy < 10: continue
                    ci = np.searchsorted(self.cmd_at, low[ok], side='right')
                    cj = np.searchsorted(self.cmd_at, high[ok], side='right')
                    counts = self.cmd_sum[cj, axis]-self.cmd_sum[ci, axis]
                    if np.count_nonzero(abs(counts) >= 2) < 6: continue
                    x = counts/seconds[ok]; x = x-x.mean(); xx, xy = x@x, x@y
                    if xx < 100: continue
                    response, quality = -xy/xx, xy*xy/(xx*yy)
                    if .1 < response < 4:
                        fits.append(self.binding(segment) | {'delay_ms': delay/1000,
                            'response_px_per_count': round(float(response), 4),
                            'r_squared': round(float(quality), 4), 'span': span, 'bins': len(x),
                            'first_id': int(self.ids[first[ok][0]]), 'last_id': int(self.ids[last[ok][-1]])})
        return sorted(fits, key=lambda f: f['r_squared'], reverse=True)[:3]

    def coverage(self):
        return self.binding() | {'frames': len(self.rows), 'first_id': int(self.ids[0]),
            'last_id': int(self.ids[-1]), 'first_observed_us': int(self.at[0]),
            'last_observed_us': int(self.at[-1]), 'missing_control_rows': self.missing,
            'integrable_segments': len(self.segments), 'valid_background_edges': int(self.edge.sum()),
            'valid_continuous_duration_ms': round(float(np.diff(self.at)[self.edge[1:]].sum()/1000), 3),
            'boundary_reasons': dict(self.reasons), 'candidate_structural_bins': self.structural_bins,
            'rejected_discontinuous_bins': self.structural_rejected,
            'rejected_command_coverage_delay_bins': len(self.rejected_command_bins),
            'command_coverage_known': self.processed is not None, 'api_accepted_reports': len(self.cmd_at)}


def analyze(rows, source):
    groups = OrderedDict()
    for row in rows:
        identity = (row.get('process_id', 'unspecified'), row.get('trace_id', 'legacy_single_trace'),
                    row.get('stream_generation', row.get('generation', 'unspecified')))
        groups.setdefault(identity, []).append(row)
    contexts = [Context(key, values) for key, values in groups.items()]
    windows, locks, overall = [], [], [[], []]
    for c in contexts:
        for axis in (0, 1): overall[axis].extend(c.fit(0, len(c.rows), axis))
        for start in np.arange(c.at[0], c.at[-1]-1000000, 2000000):
            begin, end = np.searchsorted(c.at, [start, start+2000000])
            windows.append(c.binding() | {'start_s': round((start-c.at[0])/1e6, 2),
                'x': c.fit(begin, end, 0), 'y': c.fit(begin, end, 1)})
        for segment, (begin, end) in enumerate(c.segments):
            start = begin
            for stop in range(begin+1, end+1):
                if stop < end and c.a['lock_id'][stop] == c.a['lock_id'][start]: continue
                active = (c.a['flags'][start:stop].astype(int) & (1 | 2 | 16)) == 1
                active &= c.at[start:stop]-c.at[start] >= 250000
                err = np.hypot(c.a['target_x'][start:stop]-c.centers[start:stop, 0],
                               c.a['target_y'][start:stop]-c.centers[start:stop, 1])[active]
                if len(err):
                    locks.append(c.binding(segment) | {'lock_id': int(c.a['lock_id'][start]),
                        'start_s': round((c.at[start]-c.at[0])/1e6, 3),
                        'duration_ms': round((c.at[stop-1]-c.at[start])/1000, 2), 'frames': int(active.sum()),
                        'error_p50': round(float(np.median(err)), 3), 'error_p95': round(float(np.quantile(err, .95)), 3)})
                start = stop
    return {'source': str(source), 'frames': len(rows), 'api_accepted_reports': sum(len(c.cmd_at) for c in contexts),
        'scope': 'Offline correlation, not physical HID latency or target identity. Bins and command coverage stay '
                 'inside a single continuous control-id/time/background segment. Wire-frame skipping alone is legal.',
        'overall_x': sorted(overall[0], key=lambda f: f['r_squared'], reverse=True)[:3],
        'overall_y': sorted(overall[1], key=lambda f: f['r_squared'], reverse=True)[:3],
        'window_best_delay_x_ms': dict(Counter(f['x'][0]['delay_ms'] for f in windows if f['x'] and f['x'][0]['r_squared'] >= .7)),
        'window_best_delay_y_ms': dict(Counter(f['y'][0]['delay_ms'] for f in windows if f['y'] and f['y'][0]['r_squared'] >= .7)),
        'windows': windows, 'locks_after_250ms': locks, 'coverage': [c.coverage() for c in contexts]}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('trace', type=Path); p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    source_bytes=args.trace.read_bytes()
    rows=list(csv.DictReader(io.StringIO(source_bytes.decode('utf-8-sig'))))
    if not rows: p.error('trace contains no observation rows')
    report = analyze(rows, args.trace)
    report['source_sha256']=hashlib.sha256(source_bytes).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ('windows','locks_after_250ms')}, indent=2))
