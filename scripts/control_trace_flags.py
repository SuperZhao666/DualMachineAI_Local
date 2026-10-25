"""Version-bound offline interpretation; the v6 raw flag word never changes.

The same bit has different meanings in retained releases. Missing/conflicting
identity remains unresolved, rather than borrowing a newer trace's version.
"""
from collections import Counter, defaultdict
import re


LAYOUT_CHOICES = ('auto', 'head_observation_v1', 'body_unassigned_v1', 'large_background_v1', 'unknown')
PROFILE_EVENTS = {'mobile_inference_profile_applied', 'mobile_game_model_applied', 'mobile_aim_target_applied'}


def token(detail, name):
    match = re.search(r'\b' + re.escape(name) + r'=([^\s]+)', detail)
    return match[1] if match else None


class ControlFlagInterpreter:
    def __init__(self, schema, process_id, override='auto'):
        if override not in LAYOUT_CHOICES:
            raise ValueError('unsupported control flag layout')
        self.schema, self.process_id, self.override = schema, int(process_id), override
        self.contexts = {}

    def _context(self, trace):
        key = (self.process_id, trace)
        return self.contexts.setdefault(key, {'codes': set(), 'names': set(), 'manifests': 0,
                                              'invalid_codes': 0, 'profiles': []})

    def consume(self, event):
        if event.get('process_id') != self.process_id:
            return
        trace = event.get('trace_id')
        if not trace:
            return
        detail = event.get('detail', '')
        if event.get('event') == 'runtime_session_manifest':
            context = self._context(trace)
            context['manifests'] += 1
            code = token(detail, 'version_code')
            if code is not None and re.fullmatch(r'\d+', code):
                context['codes'].add(int(code))
            else:
                context['invalid_codes'] += 1
            name = token(detail, 'app_version')
            if name:
                context['names'].add(name)
        if event.get('event') in PROFILE_EVENTS:
            context = self._context(trace)
            model, aim = token(detail, 'model'), token(detail, 'aim_target')
            applied = token(detail, 'native_applied')
            at = event.get('monotonic_us')
            # Geometry is configuration evidence, not a meaning inferred from
            # bit16384 or the model's default geometric_head_ratio.
            if isinstance(at, int):
                context['profiles'].append({'at': at, 'sequence': int(event.get('sequence', 0)),
                                            'model': model, 'aim_target': aim,
                                            'applied': applied in ('true', '1')})

    def resolve(self, trace):
        context = self._context(trace)
        codes = sorted(context['codes'])
        result = {'process_id': self.process_id, 'trace_id': trace, 'layout': None,
                  'version_codes': codes, 'app_versions': sorted(context['names']),
                  'manifest_reports': context['manifests'], 'invalid_manifest_version_reports': context['invalid_codes'],
                  'resolution': 'missing_manifest', 'evidence_level': None}
        if self.override != 'auto':
            result.update(layout=None if self.override == 'unknown' else self.override,
                          resolution='explicit_cli_override', evidence_level='caller_selected_layout')
            return result
        if len(codes) != 1 or context['invalid_codes']:
            if len(codes) > 1:
                result['resolution'] = 'conflicting_manifest_versions'
            elif context['invalid_codes']:
                result['resolution'] = 'missing_or_invalid_manifest_version'
            return result
        code = codes[0]
        for layout, rule in self.schema['control_flag_layouts'].items():
            if code in rule.get('version_codes', ()) or any(low <= code <= high for low, high in rule.get('version_code_ranges', ())):
                direct = code in rule.get('direct_source_verified_codes', ())
                result.update(layout=layout, resolution='same_process_trace_manifest_version',
                              evidence_level='direct_retained_source' if direct else 'reviewed_release_range',
                              evidence=rule['evidence'])
                return result
        result['resolution'] = 'unsupported_manifest_version'
        return result

    def _configured_aim(self, row, layout):
        if layout not in ('body_unassigned_v1', 'large_background_v1'):
            return None
        at = int(row.get('processed_us', 0))
        profiles = [p for p in self._context(row['trace_id'])['profiles'] if p['at'] <= at]
        if not profiles:
            return None
        profile = max(profiles, key=lambda p: (p['at'], p['sequence']))
        if not profile['applied'] or profile['model'] not in self.schema['control_flag_body_models']:
            return None
        return {'head': 'configured_geometric_head', 'body': 'configured_body_center'}.get(profile['aim_target'])

    def summarize(self, rows):
        rows = list(rows)
        layouts = {trace: self.resolve(trace) for trace in sorted({r['trace_id'] for r in rows})}
        raw_all, raw_active = Counter(), Counter()
        head_all, head_active = Counter(), Counter()
        aims = Counter()
        unknown_all = unknown_active = unexpected_all = unexpected_active = 0
        background_all = background_active = 0
        unresolved_raw = Counter()
        for row in rows:
            flags = int(row['flags'])
            active = bool(flags & 1) and not flags & (2 | 16)
            for bit in (16384, 32768):
                raw_all[str(bit)] += bool(flags & bit)
                if active:
                    raw_active[str(bit)] += bool(flags & bit)
            layout = layouts[row['trace_id']]['layout']
            unexpected = ((layout in ('large_background_v1', 'body_unassigned_v1') and bool(flags & 32768)) or
                          (layout == 'body_unassigned_v1' and bool(flags & 16384)) or
                          (layout == 'head_observation_v1' and flags & 49152 == 49152))
            unresolved = layout is None or unexpected
            if unresolved:
                unknown_all += 1
                unknown_active += active
                unexpected_all += bool(unexpected)
                unexpected_active += bool(unexpected and active)
                for bit in (16384, 32768):
                    unresolved_raw[str(bit)] += bool(flags & bit)
            if layout == 'head_observation_v1' and not unexpected:
                for bit, name in ((16384, 'projected'), (32768, 'measured')):
                    head_all[name] += bool(flags & bit)
                    if active:
                        head_active[name] += bool(flags & bit)
            if layout == 'large_background_v1':
                background_all += bool(flags & 16384)
                background_active += bool(active and flags & 16384)
            if active:
                if unresolved:
                    aims['unresolved'] += 1
                elif layout == 'head_observation_v1' and flags & 32768:
                    aims['measured_head_marker'] += 1
                elif layout == 'head_observation_v1' and flags & 16384:
                    aims['projected_head_marker'] += 1
                else:
                    aims[self._configured_aim(row, layout) or 'unresolved'] += 1
        return {
            'trace_layouts': list(layouts.values()),
            'raw_bit_counts': {'all_rows': {str(b): raw_all[str(b)] for b in (16384, 32768)},
                               'active_rows': {str(b): raw_active[str(b)] for b in (16384, 32768)}},
            'unresolved_rows': unknown_all, 'unresolved_active_rows': unknown_active,
            'unexpected_or_conflicting_marker_rows': unexpected_all,
            'unexpected_or_conflicting_marker_active_rows': unexpected_active,
            'unresolved_set_bit_counts': {str(b): unresolved_raw[str(b)] for b in (16384, 32768)},
            'head_source_frames': {k: None if unknown_all else head_all[k] for k in ('measured', 'projected')},
            'actionable_head_sources': {k: None if unknown_active else head_active[k] for k in ('measured', 'projected')},
            'known_interpreted_head_source_frames': {k: head_all[k] for k in ('measured', 'projected')},
            'known_interpreted_actionable_head_sources': {k: head_active[k] for k in ('measured', 'projected')},
            'large_background_motion_recovered_frames': None if unknown_all else background_all,
            'active_large_background_motion_recovered_frames': None if unknown_active else background_active,
            'known_interpreted_large_background_motion_recovered_frames': background_all,
            'known_interpreted_active_large_background_motion_recovered_frames': background_active,
            'active_aim_source_evidence': dict(aims),
            'configured_geometric_head_frames': None if aims['unresolved'] else aims['configured_geometric_head'],
            'semantics': ('Raw flags are preserved. Layout uses only the same process/trace manifest or a recorded CLI override; '
                          'missing/conflicting/unsupported identities remain null, with known partial counts separate. '
                          'Geometric/body classification requires a preceding explicit applied model/aim configuration; '
                          'it is configuration evidence, not a measured-head marker or per-frame identity ground truth.'),
        }
