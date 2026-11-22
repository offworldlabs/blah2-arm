"""Reader for blah2's IQ cache in shared memory (layout version 1).

Copy this file into a consumer; it needs only numpy. See docs/iq-cache.md.
"""

from __future__ import annotations

import mmap
import os
import struct
from dataclasses import dataclass
from typing import BinaryIO, List, Optional, Tuple

import numpy as np

DEFAULT_PATH = "/dev/shm/blah2-iq"
LAYOUT_VERSION = 1
MAGIC = b"BLAH2IQ\x00"
LAP_MARGIN = 1
UNWRITTEN = 0xFFFFFFFFFFFFFFFF

# Mirrors src/data/IqShmLayout.h.
_HEADER = struct.Struct("<8sIIQdIIIIQQQQQQQQQQ128s")
assert _HEADER.size == 256
_OFF_RUN_ID = 16
_OFF_N_WRITTEN = 104
_OFF_N_TUNING = 112
_OFF_N_INEXACT = 120
_U64 = struct.Struct("<Q")
_SLOT = struct.Struct("<QQ")
_TUNING = struct.Struct("<QBB2xIqiiiII4xdQ")
assert _TUNING.size == 64

KINDS = {0: "config", 1: "retune", 2: "gain_event"}


@dataclass(frozen=True)
class Frame:
    cpi_id: int
    timestamp: int


class LayoutError(Exception):
    """Not a blah2 IQ cache this reader understands."""


class IqCache:
    """Read-only view of the cache. Call refresh() before each batch of reads."""

    def __init__(self, path: str = DEFAULT_PATH):
        self.path = path
        self._mm: Optional[mmap.mmap] = None
        self._open()

    def _open(self) -> None:
        fd = os.open(self.path, os.O_RDONLY)
        try:
            st = os.fstat(fd)
            if st.st_size < _HEADER.size:
                raise LayoutError(f"{self.path} is too small to be an IQ cache")
            self._mm = mmap.mmap(fd, st.st_size, access=mmap.ACCESS_READ)
            self._inode = st.st_ino
        finally:
            os.close(fd)
        (magic, version, fs, run_id, cpi, n_samples, n_slots, tuning_capacity,
         tuning_entry_bytes, slot_stride, slot_table_offset, config_offset,
         config_bytes, tuning_offset, slots_offset, total_bytes, _n_written,
         _n_tuning, _n_inexact, image_tag) = _HEADER.unpack_from(self._mm, 0)
        if magic != MAGIC:
            raise LayoutError(f"{self.path} has no IQ cache magic")
        if version != LAYOUT_VERSION:
            raise LayoutError(f"{self.path} is layout version {version}, this reader knows {LAYOUT_VERSION}")
        if run_id == 0:
            raise LayoutError(f"{self.path} is being initialised; retry shortly")
        if total_bytes != len(self._mm) or tuning_entry_bytes != _TUNING.size:
            raise LayoutError(f"{self.path} does not match its own header")
        self.run_id = run_id
        self.fs = fs
        self.cpi = cpi
        self.n_samples = n_samples
        self.n_slots = n_slots
        self.image_tag = image_tag.split(b"\x00", 1)[0].decode()
        self._tuning_capacity = tuning_capacity
        self._slot_stride = slot_stride
        self._slot_table_offset = slot_table_offset
        self._tuning_offset = tuning_offset
        self._slots_offset = slots_offset
        self._frame_bytes = n_samples * 4 * 2
        self.config_text = bytes(self._mm[config_offset:config_offset + config_bytes]).decode()

    def close(self) -> None:
        if self._mm is not None:
            self._mm.close()
            self._mm = None

    def __enter__(self) -> "IqCache":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def refresh(self) -> bool:
        """Remap if blah2 restarted or replaced the file. True if it did."""
        try:
            inode = os.stat(self.path).st_ino
        except FileNotFoundError:
            inode = None
        if inode == self._inode and self._u64(_OFF_RUN_ID) == self.run_id:
            return False
        self.close()
        self._open()
        return True

    def _u64(self, offset: int) -> int:
        return _U64.unpack_from(self._mm, offset)[0]

    @property
    def n_written(self) -> int:
        return self._u64(_OFF_N_WRITTEN)

    @property
    def n_inexact(self) -> int:
        return self._u64(_OFF_N_INEXACT)

    def _oldest_readable(self, n: int) -> int:
        span = self.n_slots - LAP_MARGIN - 1
        return n - span if n > span else 0

    def _still_valid(self, cpi_id: int) -> bool:
        """The lap check, done after using the frame."""
        return (self._u64(_OFF_RUN_ID) == self.run_id
                and self.n_written < cpi_id + self.n_slots - LAP_MARGIN)

    def _slot_entry(self, cpi_id: int) -> Tuple[int, int]:
        slot = cpi_id % self.n_slots
        return _SLOT.unpack_from(self._mm, self._slot_table_offset + slot * _SLOT.size)

    def _slot_view(self, cpi_id: int) -> memoryview:
        start = self._slots_offset + (cpi_id % self.n_slots) * self._slot_stride
        return memoryview(self._mm)[start:start + self._frame_bytes]

    def index(self) -> List[Frame]:
        """Frames safe to read now, oldest first."""
        n = self.n_written
        frames = []
        for cpi_id in range(self._oldest_readable(n), n):
            entry_id, timestamp = self._slot_entry(cpi_id)
            if entry_id == cpi_id:
                frames.append(Frame(cpi_id, timestamp))
        lo = self._oldest_readable(self.n_written)
        return [f for f in frames if f.cpi_id >= lo]

    def _find(self, timestamp: int) -> Optional[int]:
        n = self.n_written
        for cpi_id in range(n - 1, self._oldest_readable(n) - 1, -1):  # newest first
            entry_id, entry_ts = self._slot_entry(cpi_id)
            if entry_id == cpi_id and entry_ts == timestamp:
                return cpi_id
        return None

    def read(self, timestamp: int) -> Optional[np.ndarray]:
        """Copy of the frame, int16 (n_samples, 4), or None if not held or lapped."""
        cpi_id = self._find(timestamp)
        if cpi_id is None:
            return None
        view = self._slot_view(cpi_id)
        samples = np.frombuffer(view, dtype="<i2").reshape(self.n_samples, 4).copy()
        del view
        return samples if self._still_valid(cpi_id) else None

    def save(self, timestamp: int, out: BinaryIO) -> Optional[Frame]:
        """Write the frame straight from shared memory. None means discard what was written."""
        cpi_id = self._find(timestamp)
        if cpi_id is None:
            return None
        view = self._slot_view(cpi_id)
        out.write(view)
        del view
        return Frame(cpi_id, timestamp) if self._still_valid(cpi_id) else None

    def tuning(self) -> List[dict]:
        """Tuning entries still held, oldest first; the first is always the config."""
        n = self._u64(_OFF_N_TUNING)
        ring = self._tuning_capacity - 1
        first = max(1, n - ring + LAP_MARGIN)
        entries = []
        for i in [0] + list(range(first, n)) if n else []:
            pos = 0 if i == 0 else 1 + (i - 1) % ring
            entries.append((i, _TUNING.unpack_from(self._mm, self._tuning_offset + pos * _TUNING.size)))
        n_after = self._u64(_OFF_N_TUNING)
        out = []
        for i, (timestamp, kind, tuner, fc, generation, gain_a, gain_b, lna, grdb,
                lna_grdb, system_gain, _reserved) in entries:
            if i != 0 and n_after - i >= ring - LAP_MARGIN:
                continue
            entry = {"index": i, "timestamp": timestamp, "kind": KINDS.get(kind, kind)}
            if kind in (0, 1):
                entry.update(fc=fc, gain_reduction_a=gain_a, gain_reduction_b=gain_b, lna_state=lna)
                if kind == 1:
                    entry["generation"] = generation
            else:
                entry.update(tuner="A" if tuner == 1 else "B", gr_db=grdb, lna_gr_db=lna_grdb,
                             system_gain_db=system_gain)
            out.append(entry)
        return out


def to_complex(samples: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    """Split a frame into complex64 reference and surveillance channels."""
    s = samples.astype(np.float32)
    return s[:, 0] + 1j * s[:, 1], s[:, 2] + 1j * s[:, 3]
