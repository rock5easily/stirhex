"""Process resource sampling for the stability tests (Issue #224).

A leak shows up as a resource count that keeps climbing while the same operation is
repeated. This module reads the counts that matter for an MFC desktop application:

- Private Bytes / Working Set - heap and mapped memory charged to the process
- GDI objects / USER objects   - pens, brushes, fonts, DCs / windows, menus, hooks
- Handles                      - kernel handles (files, events, mutexes, ...)
- Threads                      - a thread that is created per operation and never exits

All of them come from Win32 APIs read through ctypes, so no extra dependency is needed
and the numbers match what Task Manager and Process Explorer show.
"""

from __future__ import annotations

import ctypes
import time
from ctypes import wintypes
from dataclasses import dataclass, fields

KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
PSAPI = ctypes.WinDLL("psapi", use_last_error=True)
USER32 = ctypes.WinDLL("user32", use_last_error=True)

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010

GR_GDIOBJECTS = 0
GR_USEROBJECTS = 1

TH32CS_SNAPTHREAD = 0x00000004
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value

# The process is gone, or it exited between two samples. Reported as a distinct error so a
# test says "the editor died" instead of failing on a nonsense delta.
class ProcessGoneError(RuntimeError):
    """The sampled process is no longer available."""


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ThreadID", wintypes.DWORD),
        ("th32OwnerProcessID", wintypes.DWORD),
        ("tpBasePri", ctypes.c_long),
        ("tpDeltaPri", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
    ]


@dataclass(frozen=True)
class ResourceSample:
    """One reading of every counter, taken at `timestamp` (seconds since the epoch)."""

    timestamp: float
    private_bytes: int
    working_set: int
    gdi_objects: int
    user_objects: int
    handles: int
    threads: int

    # Counters that must not grow at all across a repeated operation. Memory is excluded:
    # caches, the heap's own free lists and the undo history all make it move legitimately,
    # so it is judged by its trend instead (see `slope_per_iteration`).
    EXACT_COUNTERS = ("gdi_objects", "user_objects", "handles", "threads")

    def delta(self, other: "ResourceSample") -> dict[str, int]:
        """Return self - other for every counter, keyed by field name."""
        return {
            f.name: getattr(self, f.name) - getattr(other, f.name)
            for f in fields(self)
            if f.name != "timestamp"
        }


def _thread_count(pid: int) -> int:
    """Count the threads currently owned by `pid`."""
    snapshot = KERNEL32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if snapshot == INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = THREADENTRY32()
        entry.dwSize = ctypes.sizeof(THREADENTRY32)
        if not KERNEL32.Thread32First(snapshot, ctypes.byref(entry)):
            return 0
        count = 0
        while True:
            if entry.th32OwnerProcessID == pid:
                count += 1
            if not KERNEL32.Thread32Next(snapshot, ctypes.byref(entry)):
                break
        return count
    finally:
        KERNEL32.CloseHandle(snapshot)


class ResourceProbe:
    """Samples the resource counters of one process.

    The process handle is opened once and kept, so sampling in a tight loop does not add
    an open/close pair of its own to the numbers being measured.
    """

    def __init__(self, pid: int):
        if not pid:
            raise ValueError("pid is required")
        self.pid = pid
        access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ
        handle = KERNEL32.OpenProcess(access, False, pid)
        if not handle:
            raise ProcessGoneError(
                f"Cannot open process {pid}: {ctypes.WinError(ctypes.get_last_error())}"
            )
        self._handle = handle

    def close(self) -> None:
        if self._handle:
            KERNEL32.CloseHandle(self._handle)
            self._handle = None

    def __enter__(self) -> "ResourceProbe":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def is_alive(self) -> bool:
        code = wintypes.DWORD()
        if not KERNEL32.GetExitCodeProcess(self._handle, ctypes.byref(code)):
            return False
        return code.value == 259  # STILL_ACTIVE

    def sample(self) -> ResourceSample:
        """Read every counter once."""
        if not self.is_alive():
            raise ProcessGoneError(f"Process {self.pid} has exited")

        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS_EX)
        if not PSAPI.GetProcessMemoryInfo(
            self._handle, ctypes.byref(counters), counters.cb
        ):
            raise ctypes.WinError(ctypes.get_last_error())

        handles = wintypes.DWORD()
        if not KERNEL32.GetProcessHandleCount(self._handle, ctypes.byref(handles)):
            raise ctypes.WinError(ctypes.get_last_error())

        # GetGuiResources returns 0 both for "no objects" and for failure, so the error
        # code decides which one it was.
        ctypes.set_last_error(0)
        gdi = USER32.GetGuiResources(self._handle, GR_GDIOBJECTS)
        user = USER32.GetGuiResources(self._handle, GR_USEROBJECTS)
        err = ctypes.get_last_error()
        if gdi == 0 and user == 0 and err != 0:
            raise ctypes.WinError(err)

        return ResourceSample(
            timestamp=time.time(),
            private_bytes=counters.PrivateUsage,
            working_set=counters.WorkingSetSize,
            gdi_objects=gdi,
            user_objects=user,
            handles=handles.value,
            threads=_thread_count(self.pid),
        )


def slope_per_iteration(iterations: list[int], values: list[float]) -> float:
    """Least-squares slope of `values` over `iterations` (units per iteration).

    A leak is a trend, not a single jump: a one-off allocation during warm-up raises the
    total but leaves the slope flat, while a per-operation leak shows a positive slope.
    Returns 0.0 when there is nothing to fit.
    """
    n = len(iterations)
    if n < 2 or n != len(values):
        return 0.0
    mean_x = sum(iterations) / n
    mean_y = sum(values) / n
    denominator = sum((x - mean_x) ** 2 for x in iterations)
    if denominator == 0:
        return 0.0
    numerator = sum((x - mean_x) * (y - mean_y) for x, y in zip(iterations, values))
    return numerator / denominator


def format_samples_csv(samples: list[tuple[int, ResourceSample]]) -> str:
    """Render (iteration, sample) pairs as CSV text, newest row last."""
    header = "iteration,elapsed_sec," + ",".join(
        f.name for f in fields(ResourceSample) if f.name != "timestamp"
    )
    if not samples:
        return header + "\n"
    start = samples[0][1].timestamp
    lines = [header]
    for iteration, sample in samples:
        values = [
            str(getattr(sample, f.name))
            for f in fields(ResourceSample)
            if f.name != "timestamp"
        ]
        lines.append(
            f"{iteration},{sample.timestamp - start:.3f}," + ",".join(values)
        )
    return "\n".join(lines) + "\n"
