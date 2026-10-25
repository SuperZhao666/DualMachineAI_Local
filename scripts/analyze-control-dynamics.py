"""Fit logged background response to API-accepted HID reports, never physical ACK."""
import argparse, csv, json
from collections import Counter
from pathlib import Path
import numpy as np
from control_trace_geometry import center

p = argparse.ArgumentParser()
p.add_argument('trace', type=Path)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
rows = list(csv.DictReader(args.trace.open(encoding='utf-8')))
if not rows:
    p.error('trace contains no observation rows')
centers = np.array([[center(r, 'x'), center(r, 'y')] for r in rows], dtype=float)
a = {k: np.array([float(r[k]) for r in rows]) for k in rows[0] if k != 'trace_id'}
at = a['observed_us']; dt = np.diff(at, prepend=at[0]); n = len(at)
valid = ((a['flags'].astype(int) & 8) != 0) & (dt > 0) & (dt < 20000)
cmd = {int(r['accepted_at_us']): (int(r['accepted_dx']), int(r['accepted_dy']))
       for r in rows if int(r['accepted_at_us'])}
cmd_at = np.array(sorted(cmd), dtype=np.int64)
cmd_xy = np.array([cmd[t] for t in cmd_at], dtype=float).reshape(-1, 2)
cmd_sum = np.vstack([np.zeros(2), cmd_xy.cumsum(axis=0)])
bad_sum = np.r_[0, (~valid).cumsum()]
bg_sum = np.vstack([np.zeros(2), np.column_stack([a['background_x'], a['background_y']]).cumsum(axis=0)])

def fit(begin, end, axis):
    if not len(cmd_at):
        return []
    fits = []
    for span in [2, 4, 8]:
        last = np.arange(max(span, begin + span), end, span)
        first = last - span
        ok = (bad_sum[last+1] == bad_sum[first+1]) & (at[last] > at[first])
        first, last = first[ok], last[ok]
        if len(last) < 10: continue
        seconds = (at[last] - at[first]) / 1e6
        y = (bg_sum[last+1,axis] - bg_sum[first+1,axis]) / seconds
        y = y - y.mean()
        yy = y @ y
        if yy < 10: continue
        for delay in range(0, 120001, 2000):
            ci = np.searchsorted(cmd_at, at[first]-delay, side='right')
            cj = np.searchsorted(cmd_at, at[last]-delay, side='right')
            counts = cmd_sum[cj,axis] - cmd_sum[ci,axis]
            if np.count_nonzero(abs(counts) >= 2) < 6: continue
            x = counts / seconds; x = x - x.mean()
            xx, xy = x @ x, x @ y
            if xx < 100: continue
            response, quality = -xy/xx, xy*xy/(xx*yy)
            if .1 < response < 4:
                fits.append({'delay_ms': delay/1000, 'response_px_per_count': round(float(response),4),
                             'r_squared': round(float(quality),4), 'span': span, 'bins': len(x)})
    return sorted(fits, key=lambda f:f['r_squared'], reverse=True)[:3]

windows = []
for start in np.arange(at[0], at[-1]-1000000, 2000000):
    begin, end = np.searchsorted(at, [start, start+2000000])
    windows.append({'start_s': round((start-at[0])/1e6,2),
                    'x':fit(begin,end,0), 'y':fit(begin,end,1)})

locks = []
start = 0
for end in range(1,n+1):
    if end < n and a['lock_id'][end] == a['lock_id'][start]: continue
    active = (a['flags'][start:end].astype(int) & 3) == 1
    active &= at[start:end] - at[start] >= 250000
    err = np.hypot(a['target_x'][start:end]-centers[start:end, 0],
                   a['target_y'][start:end]-centers[start:end, 1])[active]
    if len(err):
        locks.append({'lock_id':int(a['lock_id'][start]), 'start_s':round((at[start]-at[0])/1e6,3),
                      'duration_ms':round((at[end-1]-at[start])/1e3,2), 'frames':int(active.sum()),
                      'error_p50':round(float(np.median(err)),3), 'error_p95':round(float(np.quantile(err,.95)),3)})
    start = end

report = {'source':str(args.trace), 'frames':n, 'api_accepted_reports':len(cmd),
          'scope':'Offline correlation from background motion; user movement and game dynamics are confounders. Not physical HID latency or ground truth target identity.',
          'overall_x':fit(0,n,0), 'overall_y':fit(0,n,1),
          'window_best_delay_x_ms':dict(Counter(f['x'][0]['delay_ms'] for f in windows if f['x'] and f['x'][0]['r_squared']>=.7)),
          'window_best_delay_y_ms':dict(Counter(f['y'][0]['delay_ms'] for f in windows if f['y'] and f['y'][0]['r_squared']>=.7)),
          'windows':windows, 'locks_after_250ms':locks}
args.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k not in ['windows','locks_after_250ms']},indent=2))
print('locks', json.dumps(locks[:12]))
