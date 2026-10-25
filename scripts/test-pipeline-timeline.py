"""Regression cases for interpreting a bounded, asynchronous flight recorder."""
import csv
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('pipeline_timeline', Path(__file__).with_name('analyze-pipeline-timeline.py'))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class TimelineTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.row = dict(zip(analysis.FIELDS,
            [1, 1, 7, 10, 100, 200, 400, 450, 500, 600, 900, 1900, 2100, 2200, 2300, 990, 100, 90, 2, 1]))

    def save(self, rows, *, lost=0):
        encoded = ';'.join(','.join(str(row[k]) for k in analysis.FIELDS) for row in rows)
        detail = f'pipeline_trace_lost={lost} pipeline_trace_v1=[{encoded}]'
        events = [dict(process_id=9, trace_id='test', sequence=1, event='runtime_pipeline_trace_v1', detail=detail),
                  dict(process_id=9, trace_id='test', sequence=2, event='runtime_trace_batch', detail=f'pipeline_trace_lost={lost}')]
        (self.path/'visionforge-mobile-runtime.jsonl').write_text('\n'.join(map(json.dumps, events)))

    def test_per_frame_totals_nested_timings_and_exact_control_join(self):
        self.save([self.row])
        controls = self.path/'control.csv'
        with controls.open('w', newline='') as h:
            writer = csv.DictWriter(h, ['trace_id', 'sequence', 'wire_frame_id', 'observed_us', 'flags'])
            writer.writeheader(); writer.writerow(dict(trace_id='test', sequence=1, wire_frame_id=7, observed_us=100, flags=1))
        report = analysis.analyze(self.path, 9, controls)
        self.assertEqual(report['stages']['phone_receive_to_control_us']['p50'], 2100)
        self.assertEqual(report['stages']['inference_queue_and_entry_us']['p50'], 300)
        self.assertEqual(report['stages']['inference_total_us']['p50'], 1000)
        self.assertEqual(report['control_join']['active_target_frames'], 1)
        self.assertEqual(report['control_join']['unmatched_frames'], 0)

    def test_invalid_clocks_and_nested_durations_are_not_measurements(self):
        rows = [dict(self.row, id=1, received_us=99), dict(self.row, id=2, enqueued_us=0),
                dict(self.row, id=3, qnn_us=5000), dict(self.row, id=4, background_us=300)]
        self.save(rows)
        report = analysis.analyze(self.path, 9)
        self.assertEqual(report['valid_rows'], 0)
        self.assertEqual(len(report['invalid_rows']), 4)
        self.assertEqual(report['stages']['inference_total_us'], {'n': 0})

    def test_rotation_overlap_loss_and_conflicting_duplicate(self):
        self.save([self.row, dict(self.row), dict(self.row, id=2), dict(self.row, id=2, qnn_us=900)], lost=7)
        (self.path/'visionforge-mobile-runtime.jsonl.1').write_bytes((self.path/'visionforge-mobile-runtime.jsonl').read_bytes())
        report = analysis.analyze(self.path, 9)
        self.assertEqual(report['reported_lost_rows'], 7)
        self.assertEqual(report['duplicate_events'], 2)
        self.assertEqual(report['valid_rows'], 1)
        self.assertEqual(report['invalid_rows'][0]['reason'], 'conflicting_duplicate')

    def test_absent_version_does_not_fabricate_new_stage_data(self):
        self.save([])
        self.assertEqual(analysis.analyze(self.path, 9)['valid_rows'], 0)
        self.assertEqual(analysis.analyze(self.path, 10)['unique_rows'], 0)


if __name__ == '__main__':
    unittest.main()
