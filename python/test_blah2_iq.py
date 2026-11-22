"""Cross-process test of blah2_iq.py against the C++ writer. Run via test/shm/run.sh."""

import io
import os
import subprocess
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blah2_iq  # noqa: E402

WRITER = sys.argv[1] if len(sys.argv) > 1 else "build/IqShmWriter"


def pattern(frame, n):
    i = np.arange(n, dtype=np.int64)[:, None]
    c = np.arange(4, dtype=np.int64)[None, :]
    v = (frame * 7919 + i * 31 + c * 7) % 65536 - 32768
    return v.astype(np.int16)


def start(path, n_slots, n_samples, n_frames, delay_ms, run_id):
    return subprocess.Popen([WRITER, path, str(n_slots), str(n_samples), str(n_frames),
                             str(delay_ms), str(run_id)], stdout=subprocess.DEVNULL)


def wait_for(path, proc):
    for _ in range(500):
        try:
            return blah2_iq.IqCache(path)
        except (FileNotFoundError, blah2_iq.LayoutError, ValueError):
            if proc.poll() not in (None, 0):
                raise RuntimeError("writer failed")
            time.sleep(0.01)
    raise RuntimeError("cache never appeared")


def check_header_and_tuning(path):
    w = start(path, 4, 1000, 25, 0, 11)
    assert w.wait() == 0
    with blah2_iq.IqCache(path) as cache:
        assert cache.run_id == 11 and cache.fs == 2000000 and cache.cpi == 0.5
        assert cache.n_samples == 1000 and cache.n_slots == 4
        assert cache.image_tag == "blah2:shm-test"
        assert cache.config_text == "capture:\n  fs: 2000000\n"
        assert cache.n_written == 25 and cache.n_inexact == 0
        frames = cache.index()
        assert [f.cpi_id for f in frames] == [23, 24], frames
        assert all(f.timestamp == 1000 + f.cpi_id for f in frames)
        for f in frames:
            assert np.array_equal(cache.read(f.timestamp), pattern(f.cpi_id, 1000))
            buf = io.BytesIO()
            assert cache.save(f.timestamp, buf) == f
            assert buf.getvalue() == pattern(f.cpi_id, 1000).tobytes(), "save is not the .iq layout"
        assert cache.read(1000 + 22) is None, "frame inside the lap margin was returned"
        assert cache.save(1000 + 22, io.BytesIO()) is None
        tuning = cache.tuning()
        assert tuning[0] == {"index": 0, "timestamp": 500, "kind": "config", "fc": 503000000,
                             "gain_reduction_a": 40, "gain_reduction_b": 40, "lna_state": 4}, tuning[0]
        events = [t for t in tuning if t["kind"] == "gain_event"]
        assert len(events) == 3 and events[0]["tuner"] == "A" and events[1]["tuner"] == "B"
        assert events[0]["system_gain_db"] == 58.0 and events[0]["lna_gr_db"] == 6
        ref, surv = blah2_iq.to_complex(cache.read(frames[-1].timestamp))
        assert ref.dtype == np.complex64 and ref.shape == (1000,)
    print("header and tuning: ok")


def check_concurrent(path):
    n = 20000
    w = start(path, 3, n, 1500, 0, 22)
    cache = wait_for(path, w)
    accepted = refused = saved = 0
    while w.poll() is None:
        cache.refresh()
        for f in cache.index():
            samples = cache.read(f.timestamp)
            if samples is None:
                refused += 1
                continue
            assert np.array_equal(samples, pattern(f.cpi_id, n)), f"torn frame {f}"
            accepted += 1
            buf = io.BytesIO()
            if cache.save(f.timestamp, buf) is not None:
                got = np.frombuffer(buf.getvalue(), dtype="<i2").reshape(n, 4)
                assert np.array_equal(got, pattern(f.cpi_id, n)), f"torn save {f}"
                saved += 1
    assert w.wait() == 0
    assert accepted > 0, "nothing was read, so nothing was tested"
    print(f"concurrent: {accepted} read, {saved} saved, {refused} refused as lapped")
    cache.close()


class SlowFile(io.BytesIO):
    def write(self, b):
        time.sleep(0.4)
        return super().write(b)


def check_lap_detected(path):
    n = 1000
    w = start(path, 3, n, 200, 20, 33)
    cache = wait_for(path, w)
    lapped = intact = 0
    while w.poll() is None and lapped == 0:
        frames = cache.index()
        if not frames:
            continue
        f = frames[-1]
        out = SlowFile()
        result = cache.save(f.timestamp, out)
        if result is None:
            lapped += 1
        else:
            got = np.frombuffer(out.getvalue(), dtype="<i2").reshape(n, 4)
            assert np.array_equal(got, pattern(f.cpi_id, n))
            intact += 1
    w.wait()
    assert lapped > 0, "a reader lapped by the writer was not told"
    print(f"lap detection: {lapped} lapped save refused, {intact} intact")
    cache.close()


def check_restart_and_resize(path):
    assert start(path, 4, 500, 10, 0, 44).wait() == 0
    cache = blah2_iq.IqCache(path)
    assert cache.run_id == 44 and cache.index()
    inode = os.stat(path).st_ino

    assert start(path, 4, 500, 1, 0, 45).wait() == 0
    assert os.stat(path).st_ino == inode
    assert cache.refresh() and cache.run_id == 45
    assert [f.cpi_id for f in cache.index()] == [0]

    assert start(path, 6, 500, 1, 0, 46).wait() == 0
    assert os.stat(path).st_ino != inode
    assert cache.refresh() and cache.n_slots == 6 and cache.run_id == 46
    assert not cache.refresh()
    cache.close()
    print("restart and resize: ok")


def main():
    with tempfile.TemporaryDirectory() as d:
        for i, check in enumerate([check_header_and_tuning, check_concurrent,
                                   check_lap_detected, check_restart_and_resize]):
            check(os.path.join(d, f"iq-{i}"))
    print("test_blah2_iq: all checks passed")


if __name__ == "__main__":
    main()
