"""Exercise the production evaluator CLI; never invokes network/device code."""
from pathlib import Path
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
count = 0
with tempfile.TemporaryDirectory(prefix="vf-evaluation-") as tmp:
    root = Path(tmp)
    labels = root / "labels"
    labels.mkdir()
    trace = root / "trace.csv"
    trace.write_text("frame_id,class_id,confidence,x1,y1,x2,y2\n1,0,0.9,160,160,480,480\n2,-1,0,0,0,0,0\n")
    def run(expected, label_dir=labels, marker=None):
        global count
        result = subprocess.run([str(exe), "--labels", str(label_dir), "--trace", str(trace),
            "--max-frames", "2", "--require-complete"], capture_output=True, text=True)
        assert (result.returncode == 0) == expected, (result.returncode, result.stdout, result.stderr)
        if marker: assert marker in result.stderr, result.stderr
        count += 1
        return result
    run(False, root / "missing", "labels_directory_unreadable")
    (labels / "1.txt").write_text("0 0.5 0.5 0.5 0.5\n")
    run(False, marker="2.txt")
    (labels / "2.txt").write_text("")
    good = run(True)
    assert "tp=1 fp=0 fn=0" in good.stdout and "labelled_frames=2" in good.stdout, good.stdout
    for bad in ["0 .5 .5 bad_width .5", "0 .5 .5 .5", "0 .5 .5 nan .5",
                "-1 .5 .5 .5 .5", "0 .5 .5 -0.1 .5", "0 .5 .5 0 .5",
                "0 .5 .5 .5 .5 trailing", "0 .5 .5 .5abc .5", "0 2 .5 .5 .5"]:
        (labels / "1.txt").write_text("0 .5 .5 .5 .5\n" + bad + "\n")
        run(False, marker="1.txt:2:")
    (labels / "1.txt").write_text("")
    run(True)
    (labels / "2.txt").unlink()
    (labels / "2.txt").mkdir()  # Opens as unreadable file, not a valid empty label.
    run(False, marker="2.txt")
    (labels / "2.txt").rmdir()
    (labels / "2.txt").write_text("")
    trace.write_text("frame_id,class_id,confidence,x1,y1,x2,y2\n1,0,0.9,160,160,bad,480\n2,-1,0,0,0,0,0\n")
    run(False, marker="trace.csv:2:")
print(f"PASS evaluator CLI input integrity: {count} cases")
