"""Detection and cleanup of stray Stirling / StirHex processes (Issue #113).

Both the original Stirling and StirHex refuse to run twice: a second launch hands its
command line to the instance that is already there and exits immediately. A process left
over from an earlier run therefore does not merely linger - it silently swallows every
launch the driver makes afterwards, and the driver sees only a main window that never
appears. One leaked process poisons every golden test until somebody notices and kills it
by hand.

Matching is by image file name, not by full path: the single-instance mutex is named per
session, so a Stirling.exe started from any directory blocks a launch from any other.
"""

import ctypes
from ctypes import wintypes
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
PSAPI = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_TERMINATE_AND_SYNC = 0x00100001  # SYNCHRONIZE | TERMINATE
WAIT_TIMEOUT = 0x00000102

KERNEL32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
KERNEL32.OpenProcess.restype = wintypes.HANDLE
KERNEL32.CloseHandle.argtypes = (wintypes.HANDLE,)
KERNEL32.CloseHandle.restype = wintypes.BOOL
KERNEL32.QueryFullProcessImageNameW.argtypes = (
    wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
KERNEL32.QueryFullProcessImageNameW.restype = wintypes.BOOL
KERNEL32.GetProcessTimes.argtypes = (
    wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME),
    ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME))
KERNEL32.GetProcessTimes.restype = wintypes.BOOL
KERNEL32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
KERNEL32.TerminateProcess.restype = wintypes.BOOL
KERNEL32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
KERNEL32.WaitForSingleObject.restype = wintypes.DWORD

PSAPI.EnumProcesses.argtypes = (
    ctypes.POINTER(wintypes.DWORD), wintypes.DWORD, ctypes.POINTER(wintypes.DWORD))
PSAPI.EnumProcesses.restype = wintypes.BOOL

# Image names that take part in the single-instance handshake.
STIRLING_EXE_NAMES = ("stirling.exe", "stirhex.exe")

# FILETIME epoch (1601-01-01 UTC) expressed against the Unix epoch.
_FILETIME_EPOCH = datetime(1601, 1, 1, tzinfo=timezone.utc)


@dataclass(frozen=True)
class StirlingProcess:
    """A live Stirling / StirHex process."""

    pid: int
    image_path: str
    created: datetime

    @property
    def key(self) -> tuple[int, int]:
        """Identity that survives PID reuse: a recycled PID gets a new creation time."""
        return (self.pid, int(self.created.timestamp() * 1_000_000))

    def describe(self) -> str:
        local = self.created.astimezone()
        return f"PID {self.pid}  started {local:%Y-%m-%d %H:%M:%S}  {self.image_path}"


def _enum_pids() -> list[int]:
    """Return every process id in the session."""
    capacity = 1024
    while True:
        buffer = (wintypes.DWORD * capacity)()
        needed = wintypes.DWORD()
        if not PSAPI.EnumProcesses(buffer, ctypes.sizeof(buffer), ctypes.byref(needed)):
            raise ctypes.WinError(ctypes.get_last_error())
        if needed.value < ctypes.sizeof(buffer):
            return list(buffer[: needed.value // ctypes.sizeof(wintypes.DWORD)])
        # The array was filled exactly: the list may have been truncated, so grow and redo.
        capacity *= 2


def _image_path(handle: int) -> str | None:
    size = wintypes.DWORD(32768)
    buffer = ctypes.create_unicode_buffer(size.value)
    if not KERNEL32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
        return None
    return buffer.value


def _created_at(handle: int) -> datetime | None:
    creation = wintypes.FILETIME()
    exit_time = wintypes.FILETIME()
    kernel_time = wintypes.FILETIME()
    user_time = wintypes.FILETIME()
    if not KERNEL32.GetProcessTimes(handle, ctypes.byref(creation), ctypes.byref(exit_time),
                                    ctypes.byref(kernel_time), ctypes.byref(user_time)):
        return None
    ticks = (creation.dwHighDateTime << 32) | creation.dwLowDateTime
    return _FILETIME_EPOCH + timedelta(microseconds=ticks // 10)


def find_stirling_processes() -> list[StirlingProcess]:
    """Return every live Stirling / StirHex process, oldest first.

    Processes that cannot be opened (another user, elevated) are skipped: they are not ours
    to report on, and the single-instance mutex would not reach across sessions anyway.
    """
    found: list[StirlingProcess] = []
    for pid in _enum_pids():
        if pid == 0:
            continue
        handle = KERNEL32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not handle:
            continue
        try:
            path = _image_path(handle)
            if not path or Path(path).name.lower() not in STIRLING_EXE_NAMES:
                continue
            created = _created_at(handle)
            if created is None:
                continue
            found.append(StirlingProcess(pid=pid, image_path=path, created=created))
        finally:
            KERNEL32.CloseHandle(handle)
    return sorted(found, key=lambda p: p.created)


def describe_processes(processes: list[StirlingProcess], indent: str = "  ") -> str:
    """Render processes one per line for an error or warning message."""
    return "\n".join(f"{indent}{p.describe()}" for p in processes)


def terminate_process(pid: int, timeout_ms: int = 3000) -> bool:
    """Terminate one process and wait for it to go away. True if it is gone afterwards."""
    handle = KERNEL32.OpenProcess(PROCESS_TERMINATE_AND_SYNC, False, pid)
    if not handle:
        return False
    try:
        if KERNEL32.WaitForSingleObject(handle, 0) != WAIT_TIMEOUT:
            return True  # already exited
        KERNEL32.TerminateProcess(handle, 1)
        return KERNEL32.WaitForSingleObject(handle, timeout_ms) != WAIT_TIMEOUT
    finally:
        KERNEL32.CloseHandle(handle)


def terminate_processes(processes: list[StirlingProcess]) -> list[StirlingProcess]:
    """Terminate all of `processes`; return the ones that survived."""
    return [p for p in processes if not terminate_process(p.pid)]


def stop_command(processes: list[StirlingProcess]) -> str:
    """The PowerShell command that clears the given processes, for an error message."""
    ids = ", ".join(str(p.pid) for p in processes)
    return f"Stop-Process -Id {ids} -Force"
