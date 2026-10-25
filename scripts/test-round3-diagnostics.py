"""Actual CLI regressions for R3-R01/R02; synthetic input, CPU video only."""
from pathlib import Path
import csv
import json
import os
import subprocess
import sys
import tempfile
import unittest

ACTUAL = Path(__file__).resolve().parents[1]
ROOT = Path(os.environ.get('VF_TEST_REPO', ACTUAL))
sys.path.insert(0, str(ACTUAL/'build/video-analysis'))
import cv2
import numpy as np


def write_csv(path, rows):
    with path.open('w', encoding='utf-8', newline='') as stream:
        writer=csv.DictWriter(stream, list(rows[0])); writer.writeheader(); writer.writerows(rows)


def trace_rows(count=240, width=640, height=640, trace='one'):
    rng=np.random.default_rng(314159)
    x=rng.integers(-12,13,count); y=rng.integers(-12,13,count)
    x[x==0]=3; y[y==0]=-3
    return [dict(trace_id=trace,id=i+1,lock_id=1,track_id=1,flags=9,
        observed_us=1_000_000+i*7143,processed_us=1_002_000+i*7143,
        target_x=width/2-10,target_y=height/2,dx=-3,dy=0,
        velocity_x=0,velocity_y=0,predicted_error_x=-10,predicted_error_y=0,
        background_x=-.7*int(x[i-2]) if i>=2 else 0,
        background_y=-.9*int(y[i-2]) if i>=2 else 0,
        accepted_at_us=1_001_000+i*7143,accepted_dx=int(x[i]),accepted_dy=int(y[i]),
        wire_frame_id=10+3*i,box_x1=width/2-20,box_y1=height/2-50,
        box_x2=width/2+20,box_y2=height/2+50,
        coordinate_width=width,coordinate_height=height) for i in range(count)]


class CliTests(unittest.TestCase):
    def invoke(self, name, args):
        env=os.environ.copy()
        env['PYTHONPATH']=str(ACTUAL/'build/video-analysis')+os.pathsep+env.get('PYTHONPATH','')
        env['PYTHONDONTWRITEBYTECODE']='1'
        return subprocess.run([sys.executable,str(ROOT/'scripts'/name),*map(str,args)],
            capture_output=True,text=True,encoding='utf-8',errors='replace',env=env,timeout=35)

    def dynamics(self, rows):
        with tempfile.TemporaryDirectory() as value:
            path=Path(value);csv_path=path/'trace.csv';output=path/'out.json'
            write_csv(csv_path,rows)
            result=self.invoke('analyze-control-dynamics.py',[csv_path,'--output',output])
            return result,json.loads(output.read_text()) if output.exists() else None

    def test_invariants_each_center_and_actionable_scope(self):
        for w,h in [(416,416),(640,640),(1024,1024),(1024,640)]:
            with self.subTest(dimensions=(w,h)),tempfile.TemporaryDirectory() as value:
                path=Path(value);rows=trace_rows(4,w,h)
                rows[1]['dx']=3;rows[2].update(flags=3,dx=3);rows[3].update(flags=17,dx=3)
                write_csv(path/'trace.csv',rows)
                result=self.invoke('analyze-control-invariants.py',[path/'trace.csv','--output',path/'out.json'])
                self.assertEqual(result.returncode,0,result.stderr)
                report=json.loads((path/'out.json').read_text())[str(path/'trace.csv')]
                self.assertEqual(report['axes']['x']['opposite_observed_error_without_reverse_velocity'],1)
                self.assertEqual(report['visible_target_frames'],2)

    def archive(self, directory, width, height, rows, extra=()):
        collection=directory/'live';archive=directory/'detections';collection.mkdir();archive.mkdir()
        (collection/'visionforge-mobile-runtime.jsonl').write_text(json.dumps(dict(
            process_id=42,timestamp_utc='2026-10-25T10:00:00Z',
            detail='geometry_out_of_bounds_rejected=0 geometry_size_rejected=0'))+'\n')
        write_csv(archive/'detections-00.csv',rows)
        if width is not None:
            (archive/'geometry.json').write_text(json.dumps({'detection_width':width,'detection_height':height}))
        output=directory/'out.json'
        result=self.invoke('audit-control-chain-evidence.py',['--collection',collection,'--process','42',
            '--detections',archive,'--output',output,*extra])
        return result,json.loads(output.read_text()) if output.exists() else None

    def test_archive_geometry_and_explicit_legacy(self):
        for w,h in [(416,416),(640,480),(1024,640)]:
            with self.subTest(dimensions=(w,h)),tempfile.TemporaryDirectory() as value:
                boxes=[dict(frame_id=1,class_id=0,confidence=.9,x1=w-60,y1=h-120,x2=w-20,y2=h-20),
                       dict(frame_id=2,class_id=0,confidence=.9,x1=w-10,y1=h-30,x2=w+20,y2=h+10)]
                result,report=self.archive(Path(value),w,h,boxes)
                self.assertEqual(result.returncode,0,result.stderr)
                self.assertEqual(report['archive_out_of_bounds'],1)
                self.assertEqual(report['archive_partial_geometry_eligible_after_fix'],1)
                self.assertEqual(report['archive_width'],w);self.assertEqual(report['archive_height'],h)
        row=[dict(frame_id=1,class_id=0,confidence=.9,x1=200,y1=200,x2=230,y2=300)]
        with tempfile.TemporaryDirectory() as value:
            result,report=self.archive(Path(value),None,None,row)
            self.assertNotEqual(result.returncode,0);self.assertIsNone(report)
        with tempfile.TemporaryDirectory() as value:
            result,report=self.archive(Path(value),None,None,row,('--legacy-416',))
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(report['archive_geometry_source'],'explicit_legacy416')

    def render(self, directory, width, height, control_width, control_height):
        clip=directory/'video.avi'
        writer=cv2.VideoWriter(str(clip),cv2.VideoWriter_fourcc(*'MJPG'),30,(width,height))
        self.assertTrue(writer.isOpened(),'CPU MJPG video writer dependency')
        for _ in range(16):writer.write(np.full((height,width,3),20,np.uint8))
        writer.release()
        write_csv(clip.with_suffix('.csv'),[{'frame_id':i+1}for i in range(16)])
        rows=trace_rows(16,control_width,control_height)
        for i,row in enumerate(rows):row.update(wire_frame_id=i+1,target_x=control_width/2+40,
                                               target_y=control_height/2+10)
        write_csv(directory/'trace.csv',rows)
        output=directory/'render.png'
        result=self.invoke('render-control-clip.py',[clip,directory/'trace.csv','--output',output])
        return result,output

    def test_renderer_native_and_scaled_coordinates(self):
        for w,h,cw,ch in [(416,416,416,416),(640,640,640,640),(640,480,1024,640)]:
            with self.subTest(geometry=(w,h,cw,ch)),tempfile.TemporaryDirectory() as value:
                result,output=self.render(Path(value),w,h,cw,ch)
                self.assertEqual(result.returncode,0,result.stderr)
                image=cv2.imread(str(output));self.assertEqual(image.shape,(4*(h+60),4*w,3))
                self.assertTrue(np.array_equal(image[h//2,w//2],(255,255,255)))
                report=json.loads(result.stdout);self.assertEqual(report['error_units'],'control_coordinate_pixels')

    def test_invalid_dimensions_rejected_by_each_cli(self):
        for invalid in (0,-1,'nan','inf'):
            with self.subTest(invalid=invalid),tempfile.TemporaryDirectory() as value:
                directory=Path(value);rows=trace_rows(2);rows[0]['coordinate_width']=invalid
                write_csv(directory/'trace.csv',rows)
                r=self.invoke('analyze-control-invariants.py',[directory/'trace.csv','--output',directory/'out.json'])
                self.assertNotEqual(r.returncode,0)
                r,report=self.archive(directory,invalid,640,[dict(frame_id=1,class_id=0,confidence=.9,x1=2,y1=2,x2=20,y2=30)])
                self.assertNotEqual(r.returncode,0);self.assertIsNone(report)
        with tempfile.TemporaryDirectory() as value:
            directory=Path(value);r,_=self.render(directory,640,480,640,480)
            self.assertEqual(r.returncode,0,r.stderr)
            with (directory/'trace.csv').open() as stream:rows=list(csv.DictReader(stream))
            rows[0]['coordinate_width']='nan'
            write_csv(directory/'trace.csv',rows)
            result=self.invoke('render-control-clip.py',[directory/'video.avi',directory/'trace.csv','--output',directory/'bad.png'])
            self.assertNotEqual(result.returncode,0)

    def test_fit_continuous_baseline_and_log_hole(self):
        rows=trace_rows()
        result,baseline=self.dynamics(rows)
        self.assertEqual(result.returncode,0,result.stderr);self.assertTrue(baseline['overall_x'])
        dense_result,dense_report=self.dynamics([r for i,r in enumerate(rows)if i%20!=19])
        self.assertEqual(dense_result.returncode,0,dense_result.stderr)
        self.assertEqual(dense_report['overall_x'],[])
        self.assertEqual(dense_report['overall_y'],[])
        deleted=rows[:100]+rows[101:]
        self.assertLess(deleted[100]['observed_us']-deleted[99]['observed_us'],20000)
        result,report=self.dynamics(deleted)
        self.assertEqual(result.returncode,0,result.stderr)
        coverage=report['coverage'][0];self.assertEqual(coverage['missing_control_rows'],1)
        self.assertGreater(coverage['rejected_discontinuous_bins'],0)
        self.assertLess(coverage['valid_continuous_duration_ms'],baseline['coverage'][0]['valid_continuous_duration_ms'])
        for fit in report['overall_x']+report['overall_y']:
            self.assertFalse(fit['first_id']<101<fit['last_id'])
        result,multiple=self.dynamics(rows[:100]+rows[103:])
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(multiple['coverage'][0]['missing_control_rows'],3)
        previous=[dict(row,background_previous_us=rows[i-1]['observed_us'] if i else 0)
                  for i,row in enumerate(rows)]
        previous[100]['background_previous_us']=1
        result,mismatch=self.dynamics(previous)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(mismatch['coverage'][0]['boundary_reasons']['background_previous_mismatch'],1)
        result,report=self.dynamics([r for i,r in enumerate(rows)if i%20!=19])
        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(report['overall_x'],[])
        self.assertEqual(report['overall_y'],[])

    def test_fit_rejects_duplicate_and_clock_reversal(self):
        rows=trace_rows()
        for changed in (rows[:40]+[rows[39]]+rows[40:], rows[:40]+[rows[41],rows[40]]+rows[42:]):
            result,report=self.dynamics(changed)
            self.assertNotEqual(result.returncode,0);self.assertIsNone(report)
        changed=[dict(row)for row in rows]
        changed[40]['observed_us']=changed[39]['observed_us']-1
        result,report=self.dynamics(changed)
        self.assertNotEqual(result.returncode,0);self.assertIsNone(report)

    def test_fit_trace_generation_ownership_and_wire_skips(self):
        rows=trace_rows();other=[dict(r,trace_id='second')for r in rows]
        result,report=self.dynamics(rows+other)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(report['api_accepted_reports'],480)
        self.assertEqual(len(report['coverage']),2)
        self.assertEqual({r['trace_id']for r in report['locks_after_250ms']},{'one','second'})
        self.assertTrue(all(c['missing_control_rows']==0 for c in report['coverage']))
        result,report=self.dynamics([dict(r,stream_generation=1)for r in rows]+[dict(r,stream_generation=2)for r in rows])
        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(len(report['coverage']),2)

    def test_collector_preserves_interrupted_archive_before_any_adb(self):
        for recorded in ('bursts.json','detections-00.csv'):
            with self.subTest(recorded=recorded),tempfile.TemporaryDirectory() as value:
                directory=Path(value); evidence=directory/recorded
                evidence.write_bytes(b'original interrupted evidence')
                result=self.invoke('collect-detection-bursts.py',['--device','fixture-not-a-real-device',
                    '--output',directory,'--detection-width',640,'--detection-height',640])
                self.assertNotEqual(result.returncode,0)
                self.assertIn('refusing to overwrite recorded evidence',result.stderr)
                self.assertEqual(evidence.read_bytes(),b'original interrupted evidence')
                self.assertFalse((directory/'geometry.json').exists())


if __name__=='__main__':unittest.main(verbosity=2)
