"""Wait until a GUI thread of another process has finished what it was given (Issue #296).

A thread that has drained its message queue - handled every posted and sent message and
repainted - blocks in the kernel waiting for new input: thread state Waiting with wait
reason WrUserRequest (GetMessage) or UserRequest (MsgWaitForMultipleObjects). A message
posted to it wakes it before PostMessage returns, so "waiting" observed after the post
means the message has been handled.

A thread blocked in a wait that messages do not end (WaitForSingleObject on a worker)
also reads as UserRequest. It cannot handle the message yet either, so returning there
is no worse than the fixed sleep it replaces.

That replaces fixed sleeps after PostMessage / SendMessage, which were too long for the
port (a command takes a few milliseconds) and sometimes too short for the original.
What happens outside the thread's message processing is not covered: real input from
SendInput / mouse_event is delivered by the system first, timers fire later, and file
system notifications come from other threads.
"""

import ctypes
import time

import win32gui
import win32process

_SYSTEM_PROCESS_INFORMATION_CLASS = 5
_STATUS_INFO_LENGTH_MISMATCH = 0xC0000004
_THREAD_STATE_WAITING = 5
# KWAIT_REASON values of a thread blocked until something arrives: WrUserRequest (13)
# for GetMessage / WaitMessage, UserRequest (6) for MsgWaitForMultipleObjects, which the
# port's Vista-style file dialogs and COM modal loops use (Issue #296). A posted message
# wakes both before PostMessage returns.
_IDLE_WAIT_REASONS = (6, 13)


class _UnicodeString(ctypes.Structure):
    _fields_ = [
        ("Length", ctypes.c_ushort),
        ("MaximumLength", ctypes.c_ushort),
        ("Buffer", ctypes.c_void_p),
    ]


class _SystemThreadInformation(ctypes.Structure):
    _fields_ = [
        ("KernelTime", ctypes.c_longlong),
        ("UserTime", ctypes.c_longlong),
        ("CreateTime", ctypes.c_longlong),
        ("WaitTime", ctypes.c_ulong),
        ("StartAddress", ctypes.c_void_p),
        ("UniqueProcess", ctypes.c_void_p),
        ("UniqueThread", ctypes.c_void_p),
        ("Priority", ctypes.c_long),
        ("BasePriority", ctypes.c_long),
        ("ContextSwitches", ctypes.c_ulong),
        ("ThreadState", ctypes.c_ulong),
        ("WaitReason", ctypes.c_ulong),
    ]


class _SystemProcessInformation(ctypes.Structure):
    _fields_ = [
        ("NextEntryOffset", ctypes.c_ulong),
        ("NumberOfThreads", ctypes.c_ulong),
        # WorkingSetPrivateSize, HardFaultCount + NumberOfThreadsHighWatermark, CycleTime
        ("Reserved1", ctypes.c_longlong * 3),
        ("CreateTime", ctypes.c_longlong),
        ("UserTime", ctypes.c_longlong),
        ("KernelTime", ctypes.c_longlong),
        ("ImageName", _UnicodeString),
        ("BasePriority", ctypes.c_long),
        ("UniqueProcessId", ctypes.c_void_p),
        ("InheritedFromUniqueProcessId", ctypes.c_void_p),
        ("HandleCount", ctypes.c_ulong),
        ("SessionId", ctypes.c_ulong),
        ("UniqueProcessKey", ctypes.c_size_t),
        # VM_COUNTERS (11 SIZE_T-sized fields) followed by PrivatePageCount
        ("VmCounters", ctypes.c_size_t * 12),
        ("IoCounters", ctypes.c_longlong * 6),
    ]


if ctypes.sizeof(ctypes.c_void_p) == 8:
    assert ctypes.sizeof(_SystemProcessInformation) == 0x100
    assert ctypes.sizeof(_SystemThreadInformation) == 0x50

_NTDLL = ctypes.WinDLL("ntdll")
_NTDLL.NtQuerySystemInformation.restype = ctypes.c_ulong
_buffer = ctypes.create_string_buffer(1 << 20)


def _snapshot() -> ctypes.Array:
    """All processes and threads, growing the buffer until the snapshot fits."""
    global _buffer
    needed = ctypes.c_ulong(0)
    while True:
        status = _NTDLL.NtQuerySystemInformation(
            _SYSTEM_PROCESS_INFORMATION_CLASS, _buffer, len(_buffer), ctypes.byref(needed)
        )
        if status == 0:
            return _buffer
        if status != _STATUS_INFO_LENGTH_MISMATCH:
            raise OSError(f"NtQuerySystemInformation failed: 0x{status:08X}")
        _buffer = ctypes.create_string_buffer(max(needed.value + (64 << 10), 2 * len(_buffer)))


def thread_state(pid: int, tid: int) -> tuple[int, int] | None:
    """(ThreadState, WaitReason) of one thread, or None when it no longer exists."""
    buffer = _snapshot()
    base = ctypes.addressof(buffer)
    offset = 0
    while True:
        process = _SystemProcessInformation.from_address(base + offset)
        if (process.UniqueProcessId or 0) == pid:
            first = base + offset + ctypes.sizeof(_SystemProcessInformation)
            for index in range(process.NumberOfThreads):
                thread = _SystemThreadInformation.from_address(
                    first + index * ctypes.sizeof(_SystemThreadInformation)
                )
                if (thread.UniqueThread or 0) == tid:
                    return thread.ThreadState, thread.WaitReason
            return None
        if not process.NextEntryOffset:
            return None
        offset += process.NextEntryOffset


def is_waiting_for_input(pid: int, tid: int) -> bool | None:
    """True while the thread blocks for new messages, None once it is gone."""
    state = thread_state(pid, tid)
    if state is None:
        return None
    thread_state_value, wait_reason = state
    return thread_state_value == _THREAD_STATE_WAITING and wait_reason in _IDLE_WAIT_REASONS


def wait_ui_idle(hwnd: int, timeout: float = 10.0) -> None:
    """Block until the thread that owns `hwnd` has handled everything queued to it.

    The state is required twice in a row, a moment apart, so a thread that goes back
    to sleep only between two steps of its own work is not taken as finished. Returns
    at once when the window or its thread no longer exists (the process has exited).
    Raises TimeoutError when the thread stays busy for `timeout` seconds.
    """
    if not hwnd or not win32gui.IsWindow(hwnd):
        return
    tid, pid = win32process.GetWindowThreadProcessId(hwnd)
    if not tid:
        return
    deadline = time.perf_counter() + timeout
    confirmed = False
    while True:
        waiting = is_waiting_for_input(pid, tid)
        if waiting is None:
            return
        if waiting:
            if confirmed:
                return
            confirmed = True
            time.sleep(0.005)
            continue
        confirmed = False
        if time.perf_counter() >= deadline:
            raise TimeoutError(
                f"UI thread {tid} of process {pid} was still busy after {timeout:.0f}s"
            )
        time.sleep(0.002)
