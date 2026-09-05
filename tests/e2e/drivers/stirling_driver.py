import time
import os
import ctypes
from ctypes import wintypes
from pathlib import Path
import win32gui
import win32con
import win32process
from pywinauto import Application, timings, keyboard
from pywinauto.handleprops import is64bitprocess
from pywinauto.remote_memory_block import RemoteMemoryBlock
from pywinauto.sysinfo import is_x64_Python

from drivers.process_guard import (
    describe_processes,
    find_stirling_processes,
    stop_command,
)

# Standard Stirling / MFC Command IDs
CMD_FILE_NEW = 57600
CMD_FILE_OPEN = 57601
CMD_FILE_CLOSE = 57602
CMD_FILE_SAVE = 57603
CMD_FILE_SAVE_AS = 57604
CMD_APP_EXIT = 57665

CMD_EDIT_UNDO = 57643
CMD_EDIT_REDO = 57644
CMD_EDIT_CUT = 57635
CMD_EDIT_COPY = 57634
CMD_EDIT_PASTE = 57637
CMD_EDIT_SELECT_ALL = 57642
CMD_EDIT_FIND = 57640
CMD_EDIT_REPLACE = 57641

# Stirling Custom Command IDs
ID_EDIT_REDO = 32777
ID_GOTO_DATA_TOP = 32782
ID_GOTO_DATA_END = 32783
ID_JUMP = 32794
ID_GOTO_LAST_MODIFIED = 32795
ID_TOGGLE_READONLY = 32805
ID_DELETE_SELECTION = 32810
ID_FILL_SELECTION = 32811
ID_SAVE_SELECTION = 32812
ID_REVERT_FILE = 32813
ID_FIND_MISMATCH = 32818
ID_FIND_PREV = 32822
ID_FIND_NEXT = 32823
ID_COMPARE = 32824
ID_BGREP = 32825
ID_MARK_TOGGLE = 32842
ID_MARK_NEXT = 32843
ID_MARK_PREV = 32844
ID_MARK_CLEAR_ALL = 32845
ID_MARK_LIST = 32846
ID_MARK2_TOGGLE = 32866
ID_MARK_EXPORT = 33018     # 0x80FA write marks to a file (port only, issue #99)
ID_MARK_IMPORT = 33019     # 0x80FB read marks from a file (port only, issue #99)
ID_MARK3_TOGGLE = 32867
ID_CHARSET_ASCII = 32851
ID_CHARSET_SJIS = 32852
ID_CHARSET_EUC = 32853
ID_CHARSET_UNICODE = 32854
ID_BYTEORDER_LITTLE = 32859
ID_BYTEORDER_BIG = 32860
ID_SAVE_DUMP = 32864
ID_STRUCT_CARET = 32865
ID_SELECT_RANGE = 32869
ID_SYNC_SCROLL = 32863
ID_PRINT_RANGE = 32868
ID_EDIT_PASTE_HEX = 33016  # 0x80F8 paste clipboard text as hex data (port only, issue #97)
ID_FILE_PRINT_PREVIEW = 0xE109
AFX_ID_PREVIEW_CLOSE = 0xE300

# User Menu / Popups Command IDs
ID_USERMENU_BASE = 32826  # 0x803A (User Menu 1)
ID_USERMENU_1 = 32826     # 0x803A
ID_USERMENU_10 = 32835    # 0x8043
ID_TWOSTROKE_1 = 32836    # 0x8044
ID_TWOSTROKE_3 = 32838    # 0x8046
ID_SETTINGS_ENV = 32848   # 0x8050
ID_SETTINGS_EXT = 32849   # 0x8051
ID_RUN_APP = 32847        # 0x804F

# Struct Bar Command IDs
ID_STRUCT_EDIT = 32814
ID_STRUCT_EDIT_TOGGLE = 33014
ID_STRUCT_RADIX_ONE_DEF = 33006
ID_STRUCT_RADIX_ONE_S = 33007
ID_STRUCT_RADIX_ONE_U = 33008
ID_STRUCT_RADIX_ONE_H = 33009
ID_STRUCT_RADIX_ALL_DEF = 33010
ID_STRUCT_RADIX_ALL_S = 33011
ID_STRUCT_RADIX_ALL_U = 33012
ID_STRUCT_RADIX_ALL_H = 33013

# Bit Image Command IDs
ID_BITIMAGE = 33003
ID_BITIMAGE_RELOAD = 33004

# Output pane (issue #148)
ID_OUTPUT_PANE = 33000       # 0x80E8 toggle the output pane
# Control id of the output control bar. The port and the original picked different ids
# out of the AFX_IDW_* range, and the two overlap: 0xE821 is the output bar in the
# original but the bit image bar in the port. So the id alone cannot identify the bar -
# the caption has to match as well.
IDW_OUTPUT_BAR = 0xE820           # port
IDW_OUTPUT_BAR_ORIGINAL = 0xE821  # original Stirling 1.31
OUTPUT_BAR_IDS = (IDW_OUTPUT_BAR, IDW_OUTPUT_BAR_ORIGINAL)
OUTPUT_BAR_CAPTION = "アウトプット"

# Control id and caption of the bit image control bar of the port (Issue #121).
# The original has no equivalent bar - its bit image is a plain child window - so the
# id is only meaningful for StirHex. As with the output bar, the caption is matched
# too: 0xE821 is the output bar in the original.
IDW_BITIMAGE_BAR = 0xE821
BITIMAGE_BAR_CAPTION = "ビットイメージ"

# Captions of bars that become top-level windows once floating. The main-window
# lookup must never mistake one of these for the frame.
FLOATING_BAR_CAPTIONS = (BITIMAGE_BAR_CAPTION, OUTPUT_BAR_CAPTION, "構造体編集")

# Top Address Dialog Control IDs
IDC_TOPADDR_MODE_ADDRESS = 1016
IDC_TOPADDR_MODE_MARK = 1017
IDC_TOPADDR_EDIT = 1007
IDC_TOPADDR_BASE_DEC = 1020
IDC_TOPADDR_BASE_HEX = 1022
IDC_TOPADDR_MARK_LIST = 1021

# Dialog Control IDs
IDC_JUMP_EDIT = 1007
IDC_JUMP_BASE_DEC = 1016
IDC_JUMP_BASE_HEX = 1017

# Struct bar's own status statics (ported only): edit lock / charset / byte order.
IDC_STRUCT_STATUS_EDIT = 1205
IDC_STRUCT_STATUS_CS = 1206
IDC_STRUCT_STATUS_ORDER = 1207

IDC_RANGEBAR_START = 1013
IDC_RANGEBAR_END = 1014
IDC_RANGEBAR_BASE_DEC = 1016
IDC_RANGEBAR_BASE_HEX = 1017
IDC_RANGEBAR_USESEL = 1127

IDC_FILL_EDIT = 1007

IDC_SAVEDUMP_FILE = 1126
IDC_SAVEDUMP_WHOLE = 1016
IDC_SAVEDUMP_RANGE = 1017

IDC_REPL_SEARCH_HEX = 1016
IDC_REPL_SEARCH_TEXT = 1017
IDC_REPL_REPLACE_HEX = 1018
IDC_REPL_REPLACE_TEXT = 1019
IDC_REPL_RANGE_CURSOR = 1044
IDC_REPL_RANGE_ALL = 1045
IDC_REPL_RANGE_SEL = 1046
IDC_REPL_SEARCH_COMBO = 1026
IDC_REPL_REPLACE_COMBO = 1027
IDC_REPL_PREV = 1041
IDC_REPL_NEXT = 1042
IDC_REPL_ALL = 1038

# Environment Settings "User Menu" page (IDD_SETTINGS_USERMENU 183) control IDs
IDC_UM_CATEGORY = 1083
IDC_UM_MENUSET = 1084
IDC_UM_CURRENT = 1085
IDC_UM_AVAILABLE = 1086
IDC_UM_ADD = 1071
IDC_UM_DELETE = 1072
IDC_UM_SEPARATOR = 1075

# Tab control messages (property sheet page switching)
TCM_GETITEMCOUNT = 0x1304
TCM_SETCURFOCUS = 0x1330

# "Accelerator" input dialog (IDD_ACCEL_INPUT 184) - Issue #27
IDC_ACCEL_EDIT = 1007
ACCEL_DIALOG_TITLE = "アクセラレータの指定"

# Environment Settings "Key Assign" page (IDD_KEYASSIGN 139)
IDC_KA_RESET = 1000
IDC_KA_LOAD = 1001
IDC_KA_SAVE = 1002
IDC_KA_KEYLIST = 1021
IDC_KA_CTRL = 1022
IDC_KA_SHIFT = 1023
IDC_KA_FUNC_LIST = 1024
IDC_KA_FUNC_CATEGORY = 1026

# Environment Settings "Edit 1" page (IDD_SETTINGS_EDIT1 159)
IDC_ED1_CLEAR_UNDO_ON_SAVE = 1069
IDC_ED1_SUBCARET = 1070
IDC_ED1_HILIGHT_BOTH = 1088
IDC_ED1_REALTIME_BITIMAGE = 1102
IDC_ED1_UNDO_LIMIT = 1161      # undo memory limit on/off (port only, issue #102)
IDC_ED1_UNDO_MB = 1162         # the limit in MB (port only, issue #102)
IDC_ED1_UNDO_MB_SPIN = 1163
IDC_ED1_UNDO_MB_UNIT = 1164

# Environment Settings "Edit 2" page (IDD_SETTINGS_EDIT2 197)
IDC_ED2_CARET_RESTORE = 1011
IDC_ED2_DYNAMIC_MARK = 1025
IDC_ED2_MARK_AUTO_RESTORE = 1165   # mark auto restore (port only, issue #100)

# Environment Settings "File" page (IDD_SETTINGS_FILE 157)
IDC_FILE_BACKUP_CREATE = 1030
IDC_FILE_BACKUP_FOLDER_CHK = 1035
IDC_FILE_EXCL_NONE = 1016
IDC_FILE_INI_PATH = 1150       # settings file path, read-only (port only, issue #111)
IDC_FILE_INI_SOURCE = 1151     # which rule chose that path (port only, issue #111)
IDC_FILE_INI_READONLY = 1154   # shown only when the file could not be read (issue #111)

# Environment Settings "Toolbar" page (IDD_SETTINGS_TOOLBAR 178)
IDC_TBAR_CURRENT = 1021
IDC_TBAR_CATEGORY = 1026
IDC_TBAR_AVAILABLE = 1024
IDC_TBAR_ADD = 1071
IDC_TBAR_DELETE = 1072
IDC_TBAR_UP = 1073
IDC_TBAR_DOWN = 1074
IDC_TBAR_SEPARATOR = 1075

# Extension Settings dialogs (IDD_EXT_LIST 185 / IDD_EXT_RECORD 253)
IDC_EXTLIST_LIST = 1021
IDC_EXTLIST_SETTINGS = 1000
IDC_EXTLIST_ADD = 1001
IDC_EXTLIST_DELETE = 1002
IDC_EXTREC_EXT = 1089
IDC_EXTREC_COMMENT = 1090
IDC_EXTREC_SHEET = 1500
IDC_DISP_LINESIZE = 1007
IDC_DISP_ADDR_HSCROLL = 1011
IDC_DISP_OPEN_READONLY = 1015
IDC_DISP_OPEN_INSERT = 1020
IDC_DISP_OPEN_CHARMODE = 1025
IDC_DISP_CS_ASCII = 1900
IDC_DISP_CS_SJIS = 1901
IDC_DISP_CS_EUC = 1902
IDC_DISP_CS_UNICODE = 1903
IDC_DISP_CS_EBCDIC = 1904
IDC_DISP_CS_EBCIDK = 1906
IDC_DISP_RADIX_DEC = 1910
IDC_DISP_RADIX_HEX = 1911
IDC_DISP_BO_LITTLE = 1920
IDC_DISP_BO_BIG = 1921

# Mark list (IDD_MARK_LIST 140)
IDC_MARKLIST_LIST = 1021

# Help-validation dialog controls
IDC_MISMATCH_BYTE = 1007
IDC_MISMATCH_RANGE_CURSOR = 1016
IDC_MISMATCH_RANGE_ALL = 1017
IDC_MISMATCH_RANGE_SEL = 1018
IDC_MISMATCH_PREV = 1041
IDC_MISMATCH_NEXT = 1042
IDC_COMPARE_LIST = 1021
IDC_DIFFLIST_HILITE = 1011
IDC_DIFFLIST_SYNC = 1015
IDC_DIFFLIST_LIST = 1058
IDC_DIFFLIST_SWITCH = 1000
IDC_SYNC_CANDIDATE = 1021
IDC_SYNC_REGISTERED = 1024
IDC_SYNC_ADD = 1071
IDC_SYNC_REMOVE = 1115
IDC_SYNC_RESET = 1116
IDC_PRINTRANGE_PREVIEW = 1011


USER32 = ctypes.WinDLL("user32", use_last_error=True)
USER32.SendMessageW.argtypes = (
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
)
USER32.SendMessageW.restype = wintypes.LPARAM
USER32.SendMessageA.argtypes = USER32.SendMessageW.argtypes
USER32.SendMessageA.restype = wintypes.LPARAM
USER32.PostMessageW.argtypes = (
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
)
USER32.PostMessageW.restype = wintypes.BOOL
USER32.IsWindowUnicode.argtypes = (wintypes.HWND,)
USER32.IsWindowUnicode.restype = wintypes.BOOL
KERNEL32 = ctypes.WinDLL("kernel32", use_last_error=True)
KERNEL32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
KERNEL32.OpenProcess.restype = wintypes.HANDLE
KERNEL32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
KERNEL32.WaitForSingleObject.restype = wintypes.DWORD
KERNEL32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
KERNEL32.TerminateProcess.restype = wintypes.BOOL
KERNEL32.CloseHandle.argtypes = (wintypes.HANDLE,)
KERNEL32.CloseHandle.restype = wintypes.BOOL

LVM_FIRST = 0x1000
LVM_GETITEMCOUNT = LVM_FIRST + 4
LVM_GETITEMTEXTA = LVM_FIRST + 45
LVM_GETITEMTEXTW = LVM_FIRST + 115
LVIF_TEXT = 0x0001
SB_GETTEXTA = win32con.WM_USER + 2
SB_GETTEXTW = win32con.WM_USER + 13
WM_IME_CHAR = 0x0286
TEXT_BUFFER_CHARS = 2048
STATUS_TEXT_BUFFER_CHARS = 0x10000
WS_EX_MDICHILD = getattr(win32con, "WS_EX_MDICHILD", 0x40)


def _send_message_w(hwnd: int, message: int, wparam: int = 0, lparam: int = 0) -> int:
    """Send a Unicode window message without pywin32's ANSI thunk."""
    return int(USER32.SendMessageW(
        hwnd,
        message,
        ctypes.c_size_t(wparam).value,
        ctypes.c_ssize_t(lparam).value,
    ))


def _send_message_a(hwnd: int, message: int, wparam: int = 0, lparam: int = 0) -> int:
    """Send an ANSI window message for an ANSI common control."""
    return int(USER32.SendMessageA(
        hwnd,
        message,
        ctypes.c_size_t(wparam).value,
        ctypes.c_ssize_t(lparam).value,
    ))


def _is_unicode_window(hwnd: int) -> bool:
    return bool(USER32.IsWindowUnicode(hwnd))


class StirlingAlreadyRunningError(RuntimeError):
    """A launch was swallowed by an instance that was already running.

    Stirling and StirHex allow only one instance: the process we started handed its
    command line to the existing one and exited, so it never gets a window of its own
    (Issue #113).
    """


class StirlingStartupError(RuntimeError):
    """The launched process exited before showing a window, with no other instance."""


def _require_compatible_bitness(python_is_x64: bool, target_is_x64: bool) -> None:
    """Reject a pointer-width combination that cannot address target memory."""
    if target_is_x64 and not python_is_x64:
        raise RuntimeError(
            "64-bit Stirling requires 64-bit Python for cross-process automation"
        )


def _post_message_w(hwnd: int, message: int, wparam: int = 0, lparam: int = 0) -> None:
    """Post a Unicode window message without pywin32's ANSI thunk."""
    if not USER32.PostMessageW(
        hwnd,
        message,
        ctypes.c_size_t(wparam).value,
        ctypes.c_ssize_t(lparam).value,
    ):
        raise ctypes.WinError(ctypes.get_last_error())


def _control_text(hwnd: int) -> str:
    """Read window/control text through the Unicode message path."""
    length = _send_message_w(hwnd, win32con.WM_GETTEXTLENGTH)
    buf = ctypes.create_unicode_buffer(length + 1)
    _send_message_w(hwnd, win32con.WM_GETTEXT, length + 1, ctypes.addressof(buf))
    return buf.value


def _top_level_of(hwnd: int) -> int:
    """Walk up to the top-level window owning hwnd (GetAncestor GA_ROOT)."""
    return win32gui.GetAncestor(hwnd, win32con.GA_ROOT)


def _has_mdi_client(hwnd: int) -> bool:
    """Return whether a top-level window owns an MDIClient child (i.e. is the MDI frame)."""
    found = []

    def _enum(child, _):
        if win32gui.GetClassName(child) == "MDIClient":
            found.append(child)
            return False
        return True

    try:
        win32gui.EnumChildWindows(hwnd, _enum, None)
    except Exception:
        pass
    return bool(found)


def _is_mdi_document_child(hwnd: int, mdi_hwnd: int) -> bool:
    """Return whether hwnd is a visible native MDI document child."""
    if (win32gui.GetParent(hwnd) != mdi_hwnd
            or not win32gui.IsWindowVisible(hwnd)):
        return False
    exstyle = win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE) & 0xFFFFFFFF
    return bool(exstyle & WS_EX_MDICHILD)


def _set_control_text(hwnd: int, text: str) -> None:
    """Set window/control text through WM_SETTEXT on the Unicode message path."""
    buf = ctypes.create_unicode_buffer(str(text))
    ctypes.set_last_error(0)
    result = _send_message_w(hwnd, win32con.WM_SETTEXT, 0, ctypes.addressof(buf))
    if result <= 0:
        error = ctypes.get_last_error()
        if error:
            raise ctypes.WinError(error)
        raise RuntimeError(
            f"WM_SETTEXT failed for window {hwnd:#x} (result={result})"
        )


def _find_combo_string(hwnd: int, text: str, exact: bool = True) -> int:
    """Find a combo-box item with a UTF-16 search string."""
    message = win32con.CB_FINDSTRINGEXACT if exact else win32con.CB_FINDSTRING
    buf = ctypes.create_unicode_buffer(str(text))
    return _send_message_w(hwnd, message, -1, ctypes.addressof(buf))


def _listbox_texts(hwnd: int) -> list[str]:
    """Read every list-box item through the control's native text format."""
    is_unicode = _is_unicode_window(hwnd)
    send_message = _send_message_w if is_unicode else _send_message_a
    count = send_message(hwnd, win32con.LB_GETCOUNT)
    if count == win32con.LB_ERR:
        raise RuntimeError("Could not get list-box item count")

    texts = []
    for index in range(count):
        length = send_message(hwnd, win32con.LB_GETTEXTLEN, index)
        if length == win32con.LB_ERR:
            raise RuntimeError(f"Could not get list-box item length at index {index}")
        if is_unicode:
            buf = ctypes.create_unicode_buffer(length + 1)
        else:
            buf = ctypes.create_string_buffer(length + 1)
        if send_message(hwnd, win32con.LB_GETTEXT, index, ctypes.addressof(buf)) == win32con.LB_ERR:
            raise RuntimeError(f"Could not get list-box item text at index {index}")
        text = buf.value if is_unicode else buf.value.decode("cp932", errors="replace")
        texts.append(text.replace("\u200e", ""))
    return texts


def _listbox_item_data(hwnd: int) -> list[int]:
    """Read item data from a list box, including owner-draw/no-string lists."""
    count = int(win32gui.SendMessage(hwnd, win32con.LB_GETCOUNT, 0, 0))
    if count == win32con.LB_ERR:
        raise RuntimeError("Could not get list-box item count")
    return [
        int(win32gui.SendMessage(hwnd, win32con.LB_GETITEMDATA, index, 0))
        for index in range(count)
    ]


def _safe_dlg_item(dialog_hwnd: int, control_id: int) -> int:
    try:
        return int(win32gui.GetDlgItem(dialog_hwnd, control_id))
    except Exception:
        return 0


def _combobox_texts(hwnd: int) -> list[str]:
    """Read every combo-box item through the control's native text format."""
    is_unicode = _is_unicode_window(hwnd)
    send_message = _send_message_w if is_unicode else _send_message_a
    count = send_message(hwnd, win32con.CB_GETCOUNT)
    if count == win32con.CB_ERR:
        raise RuntimeError("Could not get combo-box item count")
    texts = []
    for index in range(count):
        length = send_message(hwnd, win32con.CB_GETLBTEXTLEN, index)
        if length == win32con.CB_ERR:
            raise RuntimeError(f"Could not get combo-box item length at index {index}")
        if is_unicode:
            buf = ctypes.create_unicode_buffer(length + 1)
        else:
            buf = ctypes.create_string_buffer(length + 1)
        if send_message(
            hwnd, win32con.CB_GETLBTEXT, index, ctypes.addressof(buf)
        ) == win32con.CB_ERR:
            raise RuntimeError(f"Could not get combo-box item text at index {index}")
        text = buf.value if is_unicode else buf.value.decode("cp932", errors="replace")
        texts.append(text.replace("\u200e", ""))
    return texts


def _file_dialog_edit(dialog_hwnd: int) -> int:
    """Find the filename edit without confusing it with the shell search box."""
    edits = []

    def _enum(hwnd, _):
        if win32gui.GetClassName(hwnd) == "Edit":
            edits.append((win32gui.GetDlgCtrlID(hwnd), hwnd))
        return True

    win32gui.EnumChildWindows(dialog_hwnd, _enum, None)
    # Vista-style Save As uses 1001, Open uses 1148, and classic dialogs use edt1 (1152).
    for control_id in (1001, 1148, 1152):
        for candidate_id, hwnd in edits:
            if candidate_id == control_id:
                return hwnd
    return 0


def _listview_item_text(list_view, item_index: int, subitem_index: int) -> str:
    """Read one list-view cell in its native format using remote process memory."""
    is_unicode = _is_unicode_window(list_view.handle)
    item = list_view.LVITEM()
    item.mask = LVIF_TEXT
    item.iItem = item_index
    item.iSubItem = subitem_index
    item.cchTextMax = TEXT_BUFFER_CHARS

    item_size = ctypes.sizeof(item)
    char_size = ctypes.sizeof(ctypes.c_wchar) if is_unicode else ctypes.sizeof(ctypes.c_char)
    text_size = TEXT_BUFFER_CHARS * char_size
    remote_mem = RemoteMemoryBlock(list_view, size=item_size + text_size)
    try:
        text_address = remote_mem.Address() + item_size
        item.pszText = text_address
        remote_mem.Write(item, size=item_size)
        send_message = _send_message_w if is_unicode else _send_message_a
        message = LVM_GETITEMTEXTW if is_unicode else LVM_GETITEMTEXTA
        send_message(
            list_view.handle,
            message,
            item_index,
            remote_mem.Address(),
        )

        if is_unicode:
            text = ctypes.create_unicode_buffer(TEXT_BUFFER_CHARS)
        else:
            text = ctypes.create_string_buffer(TEXT_BUFFER_CHARS)
        remote_mem.Read(text, text_address)
        return text.value if is_unicode else text.value.decode("cp932", errors="replace")
    finally:
        del remote_mem


def _statusbar_part_text(status_bar, part_index: int) -> str:
    """Read one status-bar part in its native format using a race-safe buffer."""
    is_unicode = _is_unicode_window(status_bar.handle)
    if is_unicode:
        text = ctypes.create_unicode_buffer(STATUS_TEXT_BUFFER_CHARS)
    else:
        text = ctypes.create_string_buffer(STATUS_TEXT_BUFFER_CHARS)
    remote_mem = RemoteMemoryBlock(status_bar, size=ctypes.sizeof(text))
    try:
        remote_mem.Write(text)
        send_message = _send_message_w if is_unicode else _send_message_a
        message = SB_GETTEXTW if is_unicode else SB_GETTEXTA
        send_message(
            status_bar.handle,
            message,
            part_index,
            remote_mem.Address(),
        )
        remote_mem.Read(text)
        return text.value if is_unicode else text.value.decode("cp932", errors="replace")
    finally:
        del remote_mem


def safe_set_focus(hwnd: int):
    """Safely bring window to foreground without requiring active mouse cursor."""
    try:
        if win32gui.IsIconic(hwnd):
            win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
        else:
            win32gui.ShowWindow(hwnd, win32con.SW_SHOW)
        win32gui.SetForegroundWindow(hwnd)
    except Exception:
        pass


ID_CHARSET_ASCII = 32851
ID_CHARSET_SJIS = 32852
ID_CHARSET_EUC = 32853
ID_CHARSET_UNICODE = 32854
ID_CHARSET_EBCDIC = 32856
ID_CHARSET_EBCIDK = 32857
ID_CHARSET_UTF8 = 33017    # 0x80F9 UTF-8 charset (port only, issue #98)
ID_BYTEORDER_LITTLE = 32859
ID_BYTEORDER_BIG = 32860

class StirlingDriver:
    """pywinauto + Win32 API driver for Stirling (both Original and Ported builds)."""

    def __init__(self, exe_path: str | Path):
        self.exe_path = str(Path(exe_path).resolve())
        self.app: Application | None = None
        self.main_window = None
        self.hwnd: int = 0
        self.pid: int = 0

    def _get_process_windows(self) -> list[tuple[int, str, str]]:
        """Get all visible top-level windows for this process."""
        wins = []
        def _enum(h, _):
            if win32gui.IsWindowVisible(h):
                _, p = win32process.GetWindowThreadProcessId(h)
                if p == self.pid:
                    wins.append((h, win32gui.GetClassName(h), _control_text(h)))
            return True
        win32gui.EnumWindows(_enum, None)
        return wins

    def start(self, *files: str | Path, options: list[str] | None = None) -> "StirlingDriver":
        """Start the Stirling application with optional file arguments.

        `options` are switches such as /ini:<path>, passed through verbatim and ahead of the
        files: they are not paths, so they must not be resolved like one.
        """
        cmd_parts = [f'"{self.exe_path}"']
        for opt in (options or []):
            cmd_parts.append(f'"{opt}"')
        flat_files = []
        for f in files:
            if isinstance(f, (list, tuple)):
                flat_files.extend(f)
            elif f:
                flat_files.append(f)

        for f in flat_files:
            cmd_parts.append(f'"{Path(f).resolve()}"')

        cmd = " ".join(cmd_parts)
        self.app = Application(backend="win32").start(cmd)
        self.pid = self.app.process
        try:
            _require_compatible_bitness(is_x64_Python(), is64bitprocess(self.pid))
        except Exception:
            self.app.kill()
            self.app = None
            self.pid = 0
            raise

        # Wait for main window or dialog to appear
        def _find_main():
            wins = self._get_process_windows()
            # The MDI frame is identified by its MDIClient child, not by being the first
            # top-level window: a floating control bar (the bit image window restored on
            # start, Issue #121) is a top-level window of the process too, and enumerates
            # ahead of the frame. Commands posted to it are silently dropped.
            for h, cls, title in wins:
                if cls != "#32770" and not cls.startswith("UAC") and _has_mdi_client(h):
                    return h
            # Fallback for a frame whose MDIClient is not up yet. Floating control bars
            # are excluded by caption so this cannot hand back a bar again.
            for h, cls, title in wins:
                if (cls != "#32770" and not cls.startswith("UAC")
                        and title not in FLOATING_BAR_CAPTIONS):
                    return h
            for h, cls, title in wins:
                if cls == "#32770":
                    return h
            raise RuntimeError("Main window not found yet")

        self.hwnd = self._wait_for_main_window(_find_main)
        if self.hwnd and win32gui.GetClassName(self.hwnd) != "#32770":
            self.main_window = self.app.window(handle=self.hwnd)
            safe_set_focus(self.hwnd)
        time.sleep(0.3)
        return self

    def _process_exited(self) -> bool:
        """True once the process we launched has terminated."""
        handle = KERNEL32.OpenProcess(0x00100000, False, self.pid)  # SYNCHRONIZE
        if not handle:
            return True  # gone, or no longer openable - either way it will show no window
        try:
            return KERNEL32.WaitForSingleObject(handle, 0) != 0x00000102  # WAIT_TIMEOUT
        finally:
            KERNEL32.CloseHandle(handle)

    def _wait_for_main_window(self, find_main, timeout: float = 10.0,
                              interval: float = 0.3) -> int:
        """Wait for the main window, failing fast and by name when it can never come.

        Waiting on the clock alone turns every cause into the same timeout. The launch that
        gets swallowed by an existing instance is the common one (Issue #113): the process
        exits within milliseconds, so the moment it is gone without a window we can say what
        happened and name the process that took the launch.
        """
        deadline = time.time() + timeout
        while True:
            try:
                return find_main()
            except Exception:
                pass
            if self._process_exited():
                others = [p for p in find_stirling_processes() if p.pid != self.pid]
                if others:
                    raise StirlingAlreadyRunningError(
                        f"{Path(self.exe_path).name} handed its command line to an instance "
                        f"that was already running, then exited (single-instance mutex).\n"
                        f"Running instances:\n{describe_processes(others)}\n"
                        f"Close them and run again:\n  {stop_command(others)}"
                    )
                raise StirlingStartupError(
                    f"{Path(self.exe_path).name} exited before showing a window "
                    f"(PID {self.pid}); no other instance is running."
                )
            if time.time() >= deadline:
                raise timings.TimeoutError(
                    f"Main window of {Path(self.exe_path).name} (PID {self.pid}) did not "
                    f"appear within {timeout:.0f}s; the process is still running."
                )
            time.sleep(interval)

    def get_mdi_client(self) -> int:
        """Return the application's MDI client HWND."""
        mdi_client: list[int] = []

        def _enum_mdi(hwnd, _):
            if win32gui.GetClassName(hwnd) == "MDIClient":
                mdi_client.append(hwnd)
            return True

        win32gui.EnumChildWindows(self.hwnd, _enum_mdi, None)
        return mdi_client[0] if mdi_client else 0

    def get_mdi_child_titles(self) -> list[str]:
        """Get titles of all open MDI document windows."""
        mdi = self.get_mdi_client()
        if not mdi:
            return []
        children = []
        def _enum(h, _):
            if _is_mdi_document_child(h, mdi):
                children.append(_control_text(h))
            return True
        win32gui.EnumChildWindows(mdi, _enum, None)
        return children

    def find_message_box(self, timeout: float = 3.0) -> tuple[int, str, list[str]]:
        """Find a message box dialog (#32770) belonging to this process.
        Returns (hwnd, title, text_items).
        """
        def _find():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    items = []
                    def _enum_items(ch, _):
                        items.append(_control_text(ch))
                        return True
                    win32gui.EnumChildWindows(h, _enum_items, None)
                    return h, title, items
            raise RuntimeError("Message box not found")

        return timings.wait_until_passes(timeout, 0.2, _find)

    def answer_message_box(self, button_id: int, timeout: float = 10.0) -> str:
        """Wait for a message box, answer it with button_id (IDOK=1 / IDYES=6 / IDNO=7),
        and return its text (child control texts joined by newline).
        """
        h, _title, items = self.find_message_box(timeout=timeout)
        text = "\n".join(items)
        win32gui.PostMessage(h, win32con.WM_COMMAND, button_id, 0)
        time.sleep(0.3)
        return text

    def dismiss_message_box(self, timeout: float = 3.0) -> bool:
        """Find and dismiss any message box by clicking OK or closing."""
        try:
            h, _, _ = self.find_message_box(timeout=timeout)
            win32gui.PostMessage(h, win32con.WM_COMMAND, win32con.IDOK, 0)
            time.sleep(0.3)
            return True
        except Exception:
            return False

    def get_statusbar_info(self) -> tuple[int, bool, int] | None:
        """Get status bar information: (hwnd, is_visible, part_count).
        Returns None if msctls_statusbar32 window is not found.
        """
        status_bars = []
        def _find(h, _):
            if win32gui.GetClassName(h) == "msctls_statusbar32":
                status_bars.append(h)
            return True
        win32gui.EnumChildWindows(self.hwnd, _find, None)
        if not status_bars:
            return None
        sb_h = status_bars[0]
        is_visible = bool(win32gui.IsWindowVisible(sb_h))
        # SB_GETPARTS = 0x0406
        part_count = int(win32gui.SendMessage(sb_h, 0x0406, 0, 0))
        return sb_h, is_visible, part_count

    def capture_view_pixels(self) -> bytes | None:
        """Capture the client area pixel buffer of the active CStirlingView."""
        import ctypes
        import win32ui
        vh = self.get_view_hwnd()
        if not vh or not win32gui.IsWindow(vh):
            return None
        rect = win32gui.GetClientRect(vh)
        w = rect[2] - rect[0]
        h = rect[3] - rect[1]
        if w <= 0 or h <= 0:
            return None
        hwndDC = win32gui.GetDC(vh)
        mfcDC = win32ui.CreateDCFromHandle(hwndDC)
        saveDC = mfcDC.CreateCompatibleDC()
        saveBitMap = win32ui.CreateBitmap()
        saveBitMap.CreateCompatibleBitmap(mfcDC, w, h)
        saveDC.SelectObject(saveBitMap)
        ctypes.windll.user32.PrintWindow(vh, saveDC.GetSafeHdc(), 2) # PW_CLIENTONLY = 2
        bmpstr = saveBitMap.GetBitmapBits(True)
        win32gui.DeleteObject(saveBitMap.GetHandle())
        saveDC.DeleteDC()
        mfcDC.DeleteDC()
        win32gui.ReleaseDC(vh, hwndDC)
        return bmpstr




    def close(self):
        """Close the application cleanly, or kill if needed."""
        if self.hwnd and win32gui.IsWindow(self.hwnd):
            try:
                win32gui.PostMessage(self.hwnd, win32con.WM_CLOSE, 0, 0)
                time.sleep(0.3)
            except Exception:
                pass
        if self.app and self.pid:
            # A modal dialog can disappear just before the main frame is destroyed while
            # the process remains in teardown. pywinauto.Application.kill() may then wait
            # indefinitely. Bound the graceful wait and terminate only this test process.
            process = KERNEL32.OpenProcess(0x00100001, False, self.pid)  # SYNCHRONIZE | TERMINATE
            if process:
                try:
                    if KERNEL32.WaitForSingleObject(process, 1500) == 0x00000102:
                        KERNEL32.TerminateProcess(process, 1)
                        KERNEL32.WaitForSingleObject(process, 3000)
                finally:
                    KERNEL32.CloseHandle(process)
            self.app = None
            self.main_window = None
            self.hwnd = 0
            self.pid = 0

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def post_command(self, cmd_id: int):
        """Post a WM_COMMAND asynchronously to the main frame."""
        win32gui.PostMessage(self.hwnd, win32con.WM_COMMAND, cmd_id, 0)
        time.sleep(0.3)

    def get_view_hwnd(self) -> int:
        """Find the CStirlingView window handle inside MDI Client."""
        # 1. Find MDIClient window
        mdi_client = []
        def _enum_mdi(h, _):
            if win32gui.GetClassName(h) == "MDIClient":
                mdi_client.append(h)
            return True
        win32gui.EnumChildWindows(self.hwnd, _enum_mdi, None)
        if not mdi_client:
            return self.hwnd

        # 2. Query the active MDI child. Enumeration order is not activation order
        # once several documents are open.
        child = int(win32gui.SendMessage(
            mdi_client[0], 0x0229, 0, 0  # WM_MDIGETACTIVE
        ))
        if not child:
            return self.hwnd

        # 3. Find View window inside MDI Child (direct child of MDI child)
        views = []
        def _enum_view(h, _):
            if win32gui.GetParent(h) == child:
                views.append(h)
            return True
        win32gui.EnumChildWindows(child, _enum_view, None)
        return views[0] if views else child

    def focus_view(self):
        """Bring Stirling to foreground and ensure view has keyboard focus."""
        safe_set_focus(self.hwnd)
        vhwnd = self.get_view_hwnd()
        if vhwnd and win32gui.IsWindow(vhwnd):
            try:
                win32gui.SendMessage(vhwnd, win32con.WM_LBUTTONDOWN, win32con.MK_LBUTTON, 0x00100010)
                win32gui.SendMessage(vhwnd, win32con.WM_LBUTTONUP, 0, 0x00100010)
            except Exception:
                pass
        time.sleep(0.2)


    def send_vk(self, vk: int):
        """Send a virtual key press (WM_KEYDOWN + WM_KEYUP) to the active view."""
        vhwnd = self.get_view_hwnd()
        win32gui.SendMessage(vhwnd, win32con.WM_SETFOCUS, 0, 0)
        win32gui.SendMessage(vhwnd, win32con.WM_KEYDOWN, vk, 0)
        time.sleep(0.02)
        win32gui.SendMessage(vhwnd, win32con.WM_KEYUP, vk, 0)
        time.sleep(0.05)

    def press_tab(self):
        """Toggle between Hex pane and Text pane."""
        self.send_vk(win32con.VK_TAB)

    def press_insert(self):
        """Toggle between Overwrite mode and Insert mode."""
        self.send_vk(win32con.VK_INSERT)

    def press_delete(self):
        """Delete byte at caret."""
        self.send_vk(win32con.VK_DELETE)

    def press_backspace(self):
        """Delete byte before caret."""
        self.send_vk(win32con.VK_BACK)

    def press_arrow_left(self, count: int = 1):
        for _ in range(count):
            self.send_vk(win32con.VK_LEFT)

    def press_arrow_right(self, count: int = 1):
        for _ in range(count):
            self.send_vk(win32con.VK_RIGHT)

    def press_arrow_up(self, count: int = 1):
        for _ in range(count):
            self.send_vk(win32con.VK_UP)

    def press_arrow_down(self, count: int = 1):
        for _ in range(count):
            self.send_vk(win32con.VK_DOWN)

    def select_range_by_arrows(self, count: int = 1):
        """Select range of bytes using Shift+Right arrow keys."""
        safe_set_focus(self.hwnd)
        vhwnd = self.get_view_hwnd()
        if vhwnd and win32gui.IsWindow(vhwnd):
            try:
                win32gui.SendMessage(vhwnd, win32con.WM_SETFOCUS, 0, 0)
            except Exception:
                pass
        time.sleep(0.1)
        keyboard.send_keys(f"+{{RIGHT {count}}}")
        time.sleep(0.2)

    def type_hex_chars(self, hex_string: str):
        """Type hex characters (e.g. '12AB') into the active view."""
        vhwnd = self.get_view_hwnd()
        win32gui.SendMessage(vhwnd, win32con.WM_SETFOCUS, 0, 0)
        time.sleep(0.1)
        for ch in hex_string:
            _send_message_w(vhwnd, win32con.WM_CHAR, ord(ch), 0)
            time.sleep(0.02)
        time.sleep(0.2)

    def type_text_chars(self, text_string: str):
        """Type text characters into the active view (e.g. when Text pane is active)."""
        vhwnd = self.get_view_hwnd()
        win32gui.SendMessage(vhwnd, win32con.WM_SETFOCUS, 0, 0)
        time.sleep(0.1)
        for ch in text_string:
            _send_message_w(vhwnd, win32con.WM_CHAR, ord(ch), 0)
            time.sleep(0.02)
        time.sleep(0.2)

    def type_ime_chars(self, text_string: str):
        """Send IME-confirmed Unicode characters to the active text pane."""
        vhwnd = self.get_view_hwnd()
        win32gui.SendMessage(vhwnd, win32con.WM_SETFOCUS, 0, 0)
        time.sleep(0.1)
        for ch in text_string:
            _send_message_w(vhwnd, WM_IME_CHAR, ord(ch), 0)
            time.sleep(0.02)
        time.sleep(0.2)

    def jump_to_address(self, address_str: str, is_hex: bool = True):
        """Jump to specific address using Jump Dialog (ID_JUMP = 32794)."""
        self.post_command(ID_JUMP)

        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    return h
            raise RuntimeError("Jump Dialog not found yet")

        dlg_hwnd = timings.wait_until_passes(5, 0.2, _find_dlg)
        dlg = self.app.window(handle=dlg_hwnd)

        # Select base radio first (IDC_JUMP_BASE_HEX = 1017 / IDC_JUMP_BASE_DEC = 1016)
        if is_hex:
            try:
                r_hex = dlg.child_window(control_id=IDC_JUMP_BASE_HEX)
                win32gui.SendMessage(r_hex.handle, win32con.BM_CLICK, 0, 0)
            except Exception:
                pass
        else:
            try:
                r_dec = dlg.child_window(control_id=IDC_JUMP_BASE_DEC)
                win32gui.SendMessage(r_dec.handle, win32con.BM_CLICK, 0, 0)
            except Exception:
                pass

        time.sleep(0.1)

        # Set address text
        edit = dlg.child_window(class_name="Edit")
        _set_control_text(edit.handle, str(address_str))
        time.sleep(0.1)

        # Press OK (IDOK = 1)
        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)

        def _check_closed():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if h == dlg_hwnd:
                    raise RuntimeError("Jump Dialog still open")
            return True

        timings.wait_until_passes(5, 0.2, _check_closed)
        time.sleep(0.2)

    def fill_range_dialog(self, hex_val: str):
        """Fill selected range with hex byte value (e.g. 'FF' or '00') via ID_FILL_SELECTION (32811)."""
        self.post_command(ID_FILL_SELECTION)

        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    return h
            raise RuntimeError("Fill Range Dialog not found yet")

        dlg_hwnd = timings.wait_until_passes(5, 0.2, _find_dlg)
        dlg = self.app.window(handle=dlg_hwnd)

        edit = dlg.child_window(class_name="Edit")
        _set_control_text(edit.handle, str(hex_val))
        time.sleep(0.1)

        # Press OK (IDOK = 1)
        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)

        def _check_closed():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if h == dlg_hwnd:
                    raise RuntimeError("Fill Range Dialog still open")
            return True

        timings.wait_until_passes(5, 0.2, _check_closed)
        time.sleep(0.2)

    def select_range_dialog(self, start_addr: str, end_addr: str, is_hex: bool = True):
        """Open 'Select Range' dialog (ID_SELECT_RANGE = 32869), input start & end addresses, and press OK."""
        self.post_command(ID_SELECT_RANGE)

        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    return h
            raise RuntimeError("Select Range Dialog not found yet")

        dlg_hwnd = timings.wait_until_passes(5, 0.2, _find_dlg)

        children = []
        def _enum(ch, _):
            children.append((ch, win32gui.GetDlgCtrlID(ch), win32gui.GetClassName(ch)))
            return True
        win32gui.EnumChildWindows(dlg_hwnd, _enum, None)

        # Set address base (10進 or 16進)
        base_id = IDC_RANGEBAR_BASE_HEX if is_hex else IDC_RANGEBAR_BASE_DEC
        for ch, cid, ccls in children:
            if cid == base_id and ccls == "Button":
                win32gui.SendMessage(ch, win32con.BM_CLICK, 0, 0)
                time.sleep(0.05)

        # Set Start and End edit texts
        for ch, cid, ccls in children:
            if cid == IDC_RANGEBAR_START and ccls == "Edit":
                _set_control_text(ch, str(start_addr))
            elif cid == IDC_RANGEBAR_END and ccls == "Edit":
                _set_control_text(ch, str(end_addr))

        time.sleep(0.1)

        # Press OK button (IDOK = 1)
        btn_ok = win32gui.GetDlgItem(dlg_hwnd, 1)
        if btn_ok:
            win32gui.SendMessage(btn_ok, win32con.BM_CLICK, 0, 0)
        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)

        def _check_closed():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if h == dlg_hwnd:
                    raise RuntimeError("Select Range Dialog still open")
            return True

        timings.wait_until_passes(5, 0.2, _check_closed)
        time.sleep(0.2)


    def replace_all_dialog(self, search_str: str, replace_str: str, search_is_hex: bool = True, replace_is_hex: bool = True):
        """Execute Replace All via Replace Dialog (CMD_EDIT_REPLACE = 57641)."""
        self.post_command(CMD_EDIT_REPLACE)

        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    return h
            raise RuntimeError("Replace Dialog not found yet")

        dlg_hwnd = timings.wait_until_passes(5, 0.2, _find_dlg)
        dlg = self.app.window(handle=dlg_hwnd)

        # Find Comboboxes/Edits for Search and Replace
        # Set Search string
        search_edit = dlg.child_window(control_id=IDC_REPL_SEARCH_COMBO)
        _set_control_text(search_edit.handle, str(search_str))

        # Set Replace string
        replace_edit = dlg.child_window(control_id=IDC_REPL_REPLACE_COMBO)
        _set_control_text(replace_edit.handle, str(replace_str))

        # Check Whole Data range (IDC_REPL_RANGE_ALL = 1045)
        try:
            r_all = dlg.child_window(control_id=IDC_REPL_RANGE_ALL)
            win32gui.SendMessage(r_all.handle, win32con.BM_CLICK, 0, 0)
        except Exception:
            pass

        # Set Search Hex/Text radio
        if search_is_hex:
            try:
                r_shex = dlg.child_window(control_id=IDC_REPL_SEARCH_HEX)
                win32gui.SendMessage(r_shex.handle, win32con.BM_CLICK, 0, 0)
            except Exception:
                pass
        else:
            try:
                r_stext = dlg.child_window(control_id=IDC_REPL_SEARCH_TEXT)
                win32gui.SendMessage(r_stext.handle, win32con.BM_CLICK, 0, 0)
            except Exception:
                pass

        # Set Replace Hex/Text radio
        if replace_is_hex:
            try:
                r_rhex = dlg.child_window(control_id=IDC_REPL_REPLACE_HEX)
                win32gui.SendMessage(r_rhex.handle, win32con.BM_CLICK, 0, 0)
            except Exception:
                pass
        else:
            try:
                r_rtext = dlg.child_window(control_id=IDC_REPL_REPLACE_TEXT)
                win32gui.SendMessage(r_rtext.handle, win32con.BM_CLICK, 0, 0)
            except Exception:
                pass

        time.sleep(0.1)
        # Click "Replace All" button (IDC_REPL_ALL = 1038)
        print(f"[DEBUG] Pre-click windows: {self._get_process_windows()}")
        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, IDC_REPL_ALL, 0)
        time.sleep(0.5)
        print(f"[DEBUG] Post-click windows: {self._get_process_windows()}")

        # 1. Wait until Replace dialog is closed
        def _check_replace_dlg_closed():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if h == dlg_hwnd:
                    raise RuntimeError("Replace Dialog still open")
            return True

        timings.wait_until_passes(5, 0.2, _check_replace_dlg_closed)
        time.sleep(0.2)
        print(f"[DEBUG] After Replace dialog closed windows: {self._get_process_windows()}")

        # 2. Wait for and dismiss the result MessageBox ("%d個置換しました")
        def _dismiss_result():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    def _click_btn(child_h, _):
                        if win32gui.GetClassName(child_h).lower() == "button":
                            win32gui.SendMessage(child_h, win32con.BM_CLICK, 0, 0)
                        return True
                    win32gui.EnumChildWindows(h, _click_btn, None)
                    win32gui.SendMessage(h, win32con.WM_COMMAND, 1, 0)
                    time.sleep(0.15)
                    if not win32gui.IsWindow(h) or not win32gui.IsWindowVisible(h):
                        return True
                    raise RuntimeError("Msgbox still closing")
            raise RuntimeError("Result messagebox not appeared yet")

        try:
            timings.wait_until_passes(5, 0.2, _dismiss_result)
        except Exception:
            pass

        # 3. Ensure all modal dialogs are completely closed
        def _check_no_dialogs():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    def _click_btn(child_h, _):
                        if win32gui.GetClassName(child_h).lower() == "button":
                            win32gui.SendMessage(child_h, win32con.BM_CLICK, 0, 0)
                        return True
                    win32gui.EnumChildWindows(h, _click_btn, None)
                    win32gui.SendMessage(h, win32con.WM_COMMAND, 1, 0)
                    raise RuntimeError(f"Dialog {h} still open: {title}")
            return True

        try:
            timings.wait_until_passes(5, 0.2, _check_no_dialogs)
        except Exception:
            pass

        safe_set_focus(self.hwnd)
        time.sleep(0.3)

    def select_all(self):
        """Execute Edit -> Select All (CMD_EDIT_SELECT_ALL = 57642)."""
        self.post_command(CMD_EDIT_SELECT_ALL)
        time.sleep(0.1)

    def copy(self):
        """Execute Edit -> Copy (ID_EDIT_COPY = 57634)."""
        self.post_command(CMD_EDIT_COPY)
        time.sleep(0.1)

    def _drive_file_dialog(self, path: str, timeout: float = 10.0):
        """Type `path` into the common file dialog that is up, then press its OK button."""

        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, _title in wins:
                if cls == "#32770":
                    edit_hwnd = _file_dialog_edit(h)
                    if edit_hwnd:
                        return h, edit_hwnd
            raise RuntimeError(f"file dialog not found yet, current wins: {wins}")

        dlg_hwnd, edit_hwnd = timings.wait_until_passes(timeout, 0.2, _find_dlg)
        _set_control_text(edit_hwnd, path)
        time.sleep(0.1)
        btn = win32gui.GetDlgItem(dlg_hwnd, 1)
        if btn:
            win32gui.SendMessage(btn, win32con.BM_CLICK, 0, 0)
        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)
        time.sleep(0.4)

    def mark_export(self, dest_path: str | Path):
        """Write the document's marks to dest_path (ID_MARK_EXPORT = 33018)."""
        dest_path = str(Path(dest_path).resolve())
        if os.path.exists(dest_path):
            os.remove(dest_path)
        safe_set_focus(self.hwnd)
        self.post_command(ID_MARK_EXPORT)
        self._drive_file_dialog(dest_path)

        def _check_saved():
            if os.path.exists(dest_path):
                return True
            raise RuntimeError("mark file not written yet")

        timings.wait_until_passes(5, 0.2, _check_saved)

    def mark_import(self, src_path: str | Path):
        """Start reading marks from src_path (ID_MARK_IMPORT = 33019).

        Leaves whatever message boxes the import raises (size mismatch, merge or replace,
        the completion notice) to the caller: which ones appear is what the tests assert.
        """
        safe_set_focus(self.hwnd)
        self.post_command(ID_MARK_IMPORT)
        self._drive_file_dialog(str(Path(src_path).resolve()))

    def mark_toggle(self):
        """Toggle mark at current cursor position (ID_MARK_TOGGLE = 32842)."""
        self.post_command(ID_MARK_TOGGLE)
        time.sleep(0.1)

    def mark_next(self):
        """Jump to next mark (ID_MARK_NEXT = 32843)."""
        self.post_command(ID_MARK_NEXT)
        time.sleep(0.1)

    def mark_prev(self):
        """Jump to previous mark (ID_MARK_PREV = 32844)."""
        self.post_command(ID_MARK_PREV)
        time.sleep(0.1)

    def mark_clear_all(self):
        """Clear all marks (ID_MARK_CLEAR_ALL = 32845)."""
        self.post_command(ID_MARK_CLEAR_ALL)
        time.sleep(0.1)

    def set_charset_ascii(self):
        """Switch character set to ASCII (ID_CHARSET_ASCII = 32851)."""
        self.post_command(ID_CHARSET_ASCII)
        time.sleep(0.1)

    def set_charset_sjis(self):
        """Switch character set to Shift-JIS (ID_CHARSET_SJIS = 32852)."""
        self.post_command(ID_CHARSET_SJIS)
        time.sleep(0.1)

    def set_charset_euc(self):
        """Switch character set to EUC-JP (ID_CHARSET_EUC = 32853)."""
        self.post_command(ID_CHARSET_EUC)
        time.sleep(0.1)

    def set_charset_unicode(self):
        """Switch character set to Unicode (ID_CHARSET_UNICODE = 32854)."""
        self.post_command(ID_CHARSET_UNICODE)
        time.sleep(0.1)

    def set_byteorder_little(self):
        """Switch byte order to Little Endian (ID_BYTEORDER_LITTLE = 32859)."""
        self.post_command(ID_BYTEORDER_LITTLE)
        time.sleep(0.1)

    def set_byteorder_big(self):
        """Switch byte order to Big Endian (ID_BYTEORDER_BIG = 32860)."""
        self.post_command(ID_BYTEORDER_BIG)
        time.sleep(0.1)

    def cut(self):
        """Execute Edit -> Cut (ID_EDIT_CUT = 57635)."""
        self.post_command(CMD_EDIT_CUT)
        time.sleep(0.1)

    def paste(self):
        """Execute Edit -> Paste (ID_EDIT_PASTE = 57637)."""
        self.post_command(CMD_EDIT_PASTE)
        time.sleep(0.1)

    def paste_hex(self):
        """Execute Edit -> Paste as hex text (ID_EDIT_PASTE_HEX = 33016, port only)."""
        self.post_command(ID_EDIT_PASTE_HEX)
        time.sleep(0.1)

    def undo(self):
        """Execute Edit -> Undo (ID_EDIT_UNDO = 57643)."""
        self.post_command(CMD_EDIT_UNDO)
        time.sleep(0.1)

    def redo(self):
        """Execute Edit -> Redo (ID_EDIT_REDO = 32777)."""
        self.post_command(ID_EDIT_REDO)
        time.sleep(0.1)

    def revert_file(self):
        """Revert file to initial on-disk state via ID_REVERT_FILE (32813)."""
        print(f"[REVERT] Posting ID_REVERT_FILE to hwnd {self.hwnd} (is_orig={'orig' in self.exe_path})")
        self.post_command(ID_REVERT_FILE)

        # Wait for confirm MessageBox ("編集内容を破棄してファイルを再読み込みします")
        def _dismiss_confirm():
            wins = self._get_process_windows()
            print(f"[REVERT] Current windows: {wins}")
            for h, cls, title in wins:
                if cls == "#32770":
                    print(f"[REVERT] Found dialog {h} title={title}, sending IDYES")
                    def _click_yes(child_h, _):
                        if win32gui.GetClassName(child_h).lower() == "button":
                            cid = win32gui.GetDlgCtrlID(child_h)
                            print(f"[REVERT] Button child {child_h} id={cid}")
                            if cid in (6, 1):  # IDYES=6 or IDOK=1
                                win32gui.SendMessage(child_h, win32con.BM_CLICK, 0, 0)
                        return True
                    win32gui.EnumChildWindows(h, _click_yes, None)
                    win32gui.SendMessage(h, win32con.WM_COMMAND, 6, 0)
                    time.sleep(0.15)
                    if not win32gui.IsWindow(h) or not win32gui.IsWindowVisible(h):
                        return True
                    raise RuntimeError("Confirm MessageBox still closing")
            raise RuntimeError("Confirm MessageBox not found yet")

        try:
            timings.wait_until_passes(5, 0.2, _dismiss_confirm)
        except Exception as e:
            print(f"[REVERT] Exception waiting for confirm: {e}")

        safe_set_focus(self.hwnd)
        time.sleep(0.3)
        print(f"[REVERT] After revert windows: {self._get_process_windows()}")

    def save_dump_via_dialog(self, dest_path: str | Path):
        """Save formatted text dump to dest_path via Save Dump Dialog (ID_SAVE_DUMP = 32864)."""
        dest_path = str(Path(dest_path).resolve())
        if os.path.exists(dest_path):
            os.remove(dest_path)

        self.post_command(ID_SAVE_DUMP)

        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    # Verify it has IDC_SAVEDUMP_FILE (1126)
                    try:
                        child = self.app.window(handle=h).child_window(control_id=IDC_SAVEDUMP_FILE)
                        if child.exists():
                            return h
                    except Exception:
                        pass
            raise RuntimeError("Save Dump Dialog not found yet")

        dlg_hwnd = timings.wait_until_passes(5, 0.2, _find_dlg)
        dlg = self.app.window(handle=dlg_hwnd)

        edit = dlg.child_window(control_id=IDC_SAVEDUMP_FILE)
        _set_control_text(edit.handle, dest_path)
        time.sleep(0.1)

        # Press OK (IDOK = 1)
        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)

        def _check_saved():
            if os.path.exists(dest_path) and os.path.getsize(dest_path) >= 0:
                return True
            raise RuntimeError("Dump file not saved yet")

        timings.wait_until_passes(5, 0.2, _check_saved)
        time.sleep(0.3)

    def save_selection_via_dialog(self, dest_path: str | Path):
        """Save currently selected range to binary file via ID_SAVE_SELECTION (32812)."""
        dest_path = str(Path(dest_path).resolve())
        if os.path.exists(dest_path):
            os.remove(dest_path)

        self.post_command(ID_SAVE_SELECTION)

        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    edit_hwnd = _file_dialog_edit(h)
                    if edit_hwnd:
                        return h, edit_hwnd
            raise RuntimeError("Save Selection Dialog not found yet")

        dlg_hwnd, edit_hwnd = timings.wait_until_passes(5, 0.2, _find_dlg)
        _set_control_text(edit_hwnd, dest_path)
        time.sleep(0.1)

        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)

        def _check_saved():
            if os.path.exists(dest_path) and os.path.getsize(dest_path) >= 0:
                return True
            raise RuntimeError("Selection file not saved yet")

        timings.wait_until_passes(5, 0.2, _check_saved)
        time.sleep(0.3)

    def save_as_via_dialog(self, dest_path: str | Path):
        """Save current document to dest_path via Save As command (57604)."""
        dest_path = str(Path(dest_path).resolve())
        if os.path.exists(dest_path):
            os.remove(dest_path)

        safe_set_focus(self.hwnd)
        time.sleep(0.3)

        print(f"[SAVE_AS] Posting CMD_FILE_SAVE_AS to {self.hwnd}, current wins: {self._get_process_windows()}")
        # Trigger Save As command asynchronously
        self.post_command(CMD_FILE_SAVE_AS)

        # Wait for Save As dialog (#32770)
        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    edit_hwnd = _file_dialog_edit(h)
                    if edit_hwnd:
                        return h, edit_hwnd
            raise RuntimeError(f"Save As Dialog not found yet, current wins: {wins}")

        dlg_hwnd, edit_hwnd = timings.wait_until_passes(10, 0.2, _find_dlg)
        _set_control_text(edit_hwnd, dest_path)
        time.sleep(0.1)

        # Trigger Save button (IDOK = 1)
        btn = win32gui.GetDlgItem(dlg_hwnd, 1)
        if btn:
            win32gui.SendMessage(btn, win32con.BM_CLICK, 0, 0)
        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)
        
        # Wait until dialog closes and destination file exists
        def _check_saved():
            if os.path.exists(dest_path) and os.path.getsize(dest_path) >= 0:
                return True
            raise RuntimeError("File not saved yet")

        timings.wait_until_passes(5, 0.2, _check_saved)
        time.sleep(0.3)

    def open_file_via_dialog(self, file_path: str | Path):
        """Open a file via File Open command (57601) and Common File Dialog."""
        file_path = str(Path(file_path).resolve())
        safe_set_focus(self.hwnd)
        time.sleep(0.3)

        self.post_command(CMD_FILE_OPEN)

        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770":
                    edit_hwnd = _file_dialog_edit(h)
                    if edit_hwnd:
                        return h, edit_hwnd
            raise RuntimeError(f"File Open Dialog not found yet, current wins: {wins}")

        dlg_hwnd, edit_hwnd = timings.wait_until_passes(10, 0.2, _find_dlg)
        _set_control_text(edit_hwnd, file_path)
        time.sleep(0.1)

        # Trigger Open button (IDOK = 1)
        btn = win32gui.GetDlgItem(dlg_hwnd, 1)
        if btn:
            win32gui.SendMessage(btn, win32con.BM_CLICK, 0, 0)
        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)

        # Wait until dialog closes
        def _check_closed():
            wins = self._get_process_windows()
            for h, cls, _ in wins:
                if h == dlg_hwnd:
                    raise RuntimeError("File Open dialog still open")
            return True

        timings.wait_until_passes(5, 0.2, _check_closed)
        time.sleep(0.5)

    def find_struct_bar_controls(self) -> dict[str, int]:
        """Find handles of struct bar and its child controls (supports both Original and Ported)."""
        res = {}
        def _inspect(h):
            cls = win32gui.GetClassName(h)
            cid = win32gui.GetDlgCtrlID(h)
            txt = _control_text(h)
            
            # Struct bar dialog or frame
            if any(k in txt for k in ["構造体編集", "Struct"]):
                if cls in ["#32770", "AfxControlBar42s", "AfxControlBar140s"]:
                    res["bar"] = h
            
            if cls == "ComboBox" and cid in (1053, 1201):
                res["combo"] = h
            elif cls == "Static" and cid in (1029, 1203):
                res["addr"] = h
            elif cls in ("SysListView32", "AfxWnd42s") and cid in (1058, 1202, 1):
                res["list"] = h
            elif cls == "SysHeader32":
                res["header"] = h
            elif cls == "Button":
                if cid in (1123, 1210) or txt == "<<":
                    res["prev_rec"] = h
                elif cid in (1124, 1211) or txt == "<":
                    res["prev_byte"] = h
                elif cid in (1122, 1214) or txt == ">>":
                    res["next_rec"] = h
                elif cid in (1125, 1213) or txt == ">":
                    res["next_byte"] = h
                elif cid in (1056, 1212) or any(k in txt for k in ["移動", "Goto"]):
                    res["goto"] = h
                elif cid in (1114, 1200) or any(k in txt for k in ["再読込", "Reload"]):
                    res["reload"] = h
                elif cid in (1057, 2) or any(k in txt for k in ["閉じる", "Close"]):
                    res["close"] = h
        def _enum(h, _):
            _inspect(h)
            return True

        # Docked bars are descendants of the main frame. A floating MFC control bar is a
        # separate top-level window owned by the same process, so inspect every visible
        # process window and its descendants as well (the StirHex default is floating).
        _inspect(self.hwnd)
        win32gui.EnumChildWindows(self.hwnd, _enum, None)
        for top_hwnd, _, _ in self._get_process_windows():
            if top_hwnd == self.hwnd:
                continue
            _inspect(top_hwnd)
            win32gui.EnumChildWindows(top_hwnd, _enum, None)
        return res

    def struct_status_texts(self) -> dict[str, str]:
        """Return the struct bar's own status texts (ported only).

        Keys: "edit" (edit lock), "charset", "order" (byte order).  A control
        that is not present is omitted.
        """
        wanted = {
            IDC_STRUCT_STATUS_EDIT: "edit",
            IDC_STRUCT_STATUS_CS: "charset",
            IDC_STRUCT_STATUS_ORDER: "order",
        }
        found: dict[str, str] = {}

        def _enum(h, _):
            key = wanted.get(win32gui.GetDlgCtrlID(h))
            if key is not None and win32gui.GetClassName(h) == "Static":
                found[key] = _control_text(h)
            return True

        win32gui.EnumChildWindows(self.hwnd, _enum, None)
        for top_hwnd, _, _ in self._get_process_windows():
            if top_hwnd == self.hwnd:
                continue
            win32gui.EnumChildWindows(top_hwnd, _enum, None)
        return found

    def is_struct_bar_visible(self) -> bool:
        """Check if Struct Bar is currently visible."""
        ctrls = self.find_struct_bar_controls()
        combo = ctrls.get("combo")
        if combo and win32gui.IsWindowVisible(combo):
            return True
        return False

    def toggle_struct_bar(self, show: bool | None = None):
        """Toggle or set visibility of Struct Bar (via ID_STRUCT_EDIT / ID_STRUCT_EDIT_TOGGLE)."""
        current = self.is_struct_bar_visible()
        if show is not None and show == current:
            return
        # Post command (both 32814 and 33014 work appropriately in their respective builds)
        self.post_command(ID_STRUCT_EDIT)
        time.sleep(0.4)

    def select_struct_type(self, name_or_index: str | int):
        """Select a struct template from combo box in struct bar."""
        ctrls = self.find_struct_bar_controls()
        combo = ctrls.get("combo")
        bar = ctrls.get("bar")
        if not combo:
            raise RuntimeError("Struct bar combo box not found")
        
        parent = win32gui.GetParent(combo)
        cid = win32gui.GetDlgCtrlID(combo)
        
        if isinstance(name_or_index, int):
            idx = name_or_index
        else:
            idx = _find_combo_string(combo, str(name_or_index))
            if idx == win32con.CB_ERR:
                idx = _find_combo_string(combo, str(name_or_index), exact=False)
            if idx == win32con.CB_ERR:
                raise ValueError(f"Struct type '{name_or_index}' not found in combo box")

        win32gui.SendMessage(combo, win32con.CB_SETCURSEL, idx, 0)
        if parent:
            win32gui.SendMessage(parent, win32con.WM_COMMAND, (win32con.CBN_SELCHANGE << 16) | cid, combo)
        if bar and bar != parent:
            win32gui.SendMessage(bar, win32con.WM_COMMAND, (win32con.CBN_SELCHANGE << 16) | cid, combo)
        time.sleep(0.3)

    def struct_type_names(self) -> list[str]:
        """Return all structure names currently loaded in the struct bar."""
        combo = self.find_struct_bar_controls().get("combo")
        if not combo:
            raise RuntimeError("Struct bar combo box not found")
        return _combobox_texts(combo)

    def reload_struct_def(self):
        """Reload Struct.def without blocking when the parser opens a message box."""
        button = self.find_struct_bar_controls().get("reload")
        if not button:
            raise RuntimeError("Struct bar reload button not found")
        win32gui.PostMessage(button, win32con.BM_CLICK, 0, 0)
        time.sleep(0.3)

    def get_struct_address(self) -> str:
        """Get the current base address displayed in Struct Bar (e.g. '00000000')."""
        ctrls = self.find_struct_bar_controls()
        addr_hwnd = ctrls.get("addr")
        if not addr_hwnd:
            raise RuntimeError("Struct bar address display not found")
        return _control_text(addr_hwnd).strip()

    def _click_struct_button(self, btn_key: str):
        """Helper to click a struct bar button."""
        ctrls = self.find_struct_bar_controls()
        btn = ctrls.get(btn_key)
        if not btn:
            raise RuntimeError(f"Struct bar '{btn_key}' button not found")
        win32gui.SendMessage(btn, win32con.BM_CLICK, 0, 0)
        time.sleep(0.2)

    def struct_nav_prev_rec(self):
        """Click '<<' button (struct size previous)."""
        self._click_struct_button("prev_rec")

    def struct_nav_prev_byte(self):
        """Click '<' button (1 byte previous)."""
        self._click_struct_button("prev_byte")

    def struct_nav_next_byte(self):
        """Click '>' button (1 byte next)."""
        self._click_struct_button("next_byte")

    def struct_nav_next_rec(self):
        """Click '>>' button (struct size next)."""
        self._click_struct_button("next_rec")

    def struct_goto_dialog(self, mode: str = "address", address: str | None = None, mark_index: int = 0, is_hex: bool = True):
        """Open '先頭アドレスの指定' dialog via '移動' button, configure, and submit."""
        ctrls = self.find_struct_bar_controls()
        btn = ctrls.get("goto")
        if not btn:
            raise RuntimeError("Struct bar '移動' button not found")
        
        # Click Goto button with PostMessage to avoid blocking modal dialog
        win32gui.PostMessage(btn, win32con.BM_CLICK, 0, 0)
        time.sleep(0.3)

        def _find_dlg():
            wins = self._get_process_windows()
            for h, cls, title in wins:
                if cls == "#32770" and any(k in title for k in ["先頭アドレス", "Address", "Goto"]):
                    return h
            raise RuntimeError("Goto address dialog not found")

        dlg_hwnd = timings.wait_until_passes(5, 0.2, _find_dlg)

        if mode == "address":
            # Select Address radio
            btn_mode = win32gui.GetDlgItem(dlg_hwnd, IDC_TOPADDR_MODE_ADDRESS)
            if btn_mode:
                win32gui.SendMessage(btn_mode, win32con.BM_CLICK, 0, 0)
                time.sleep(0.1)

            # Select Base
            base_id = IDC_TOPADDR_BASE_HEX if is_hex else IDC_TOPADDR_BASE_DEC
            btn_base = win32gui.GetDlgItem(dlg_hwnd, base_id)
            if btn_base:
                win32gui.SendMessage(btn_base, win32con.BM_CLICK, 0, 0)
                time.sleep(0.1)

            # Input address
            edit = win32gui.GetDlgItem(dlg_hwnd, IDC_TOPADDR_EDIT)
            if edit and address is not None:
                _set_control_text(edit, str(address))
                time.sleep(0.1)
        elif mode == "mark":
            # Select Mark radio
            btn_mode = win32gui.GetDlgItem(dlg_hwnd, IDC_TOPADDR_MODE_MARK)
            if btn_mode:
                win32gui.SendMessage(btn_mode, win32con.BM_CLICK, 0, 0)
                time.sleep(0.1)

            # Select mark in list box
            lb = win32gui.GetDlgItem(dlg_hwnd, IDC_TOPADDR_MARK_LIST)
            if lb:
                win32gui.SendMessage(lb, win32con.LB_SETCURSEL, mark_index, 0)
                time.sleep(0.1)

        # Click OK
        btn_ok = win32gui.GetDlgItem(dlg_hwnd, 1)
        if btn_ok:
            win32gui.PostMessage(btn_ok, win32con.BM_CLICK, 0, 0)
        time.sleep(0.4)

    def set_struct_address_to_caret(self):
        """Execute 'キャレット位置を構造体編集' (32865)."""
        self.post_command(ID_STRUCT_CARET)
        time.sleep(0.3)

    def get_struct_list_texts(self) -> list[tuple[str, str, str]]:
        """Get visible rows from struct list view as (type, name, value) tuples."""
        ctrls = self.find_struct_bar_controls()
        list_hwnd = ctrls.get("list")
        if not list_hwnd:
            return []
        
        cls = win32gui.GetClassName(list_hwnd)
        if cls == "SysListView32":
            lv = self.app.window(handle=list_hwnd).wrapper_object()
            item_count = _send_message_w(list_hwnd, LVM_GETITEMCOUNT)
            column_count = lv.column_count()
            if column_count < 3:
                raise RuntimeError(
                    f"Struct list view has {column_count} columns; expected at least 3"
                )
            return [
                tuple(_listview_item_text(lv, item_index, subitem_index)
                      for subitem_index in range(3))
                for item_index in range(item_count)
            ]
        return []

    def get_struct_column_widths(self) -> list[int]:
        """Get column widths of the struct list header."""
        ctrls = self.find_struct_bar_controls()
        list_hwnd = ctrls.get("list")
        header_hwnd = ctrls.get("header")
        
        if not header_hwnd and list_hwnd:
            def _find_hdr(h, _):
                if win32gui.GetClassName(h) == "SysHeader32":
                    ctrls["header"] = h
                return True
            win32gui.EnumChildWindows(list_hwnd, _find_hdr, None)
            header_hwnd = ctrls.get("header")

        if header_hwnd:
            cnt = win32gui.SendMessage(header_hwnd, 0x1200, 0, 0) # HDM_GETITEMCOUNT
            widths = []
            for i in range(cnt):
                # HDITEM / LVM_GETCOLUMNWIDTH
                if list_hwnd:
                    w = win32gui.SendMessage(list_hwnd, 0x1000 + 29, i, 0) # LVM_GETCOLUMNWIDTH
                    widths.append(w)
            return widths
        return []

    def struct_open_row_context_menu(self, row_index: int = 0) -> int:
        """Open context menu for a struct list row via PostMessage right click."""
        ctrls = self.find_struct_bar_controls()
        list_hwnd = ctrls.get("list")
        if not list_hwnd:
            raise RuntimeError("Struct bar list control not found")

        # Calculate row click position (header height approx 24px, row height approx 16px)
        x = 60
        y = 28 + row_index * 16
        lparam = (int(y) << 16) | (int(x) & 0xFFFF)

        # Select row first with left click, then right click to popup context menu
        win32gui.PostMessage(list_hwnd, win32con.WM_LBUTTONDOWN, win32con.MK_LBUTTON, lparam)
        win32gui.PostMessage(list_hwnd, win32con.WM_LBUTTONUP, 0, lparam)
        time.sleep(0.15)
        win32gui.PostMessage(list_hwnd, win32con.WM_RBUTTONDOWN, win32con.MK_RBUTTON, lparam)
        win32gui.PostMessage(list_hwnd, win32con.WM_RBUTTONUP, 0, lparam)
        time.sleep(0.3)

        popup = self.find_popup_menu(timeout=2.0)
        if not popup:
            raise RuntimeError("Struct bar row context menu not displayed")
        return popup

    def struct_set_radix_all(self, radix: str, row_index: int = 0):
        """Set struct list radix for ALL fields via context menu (A -> S/U/H/D).
        radix can be 'hex' ('h'), 'dec_signed' ('s'), 'dec_unsigned' ('u'), 'default' ('d')."""
        key_map = {
            "hex": 'h',
            "h": 'h',
            "dec_signed": 's',
            "dec": 's',
            "s": 's',
            "dec_unsigned": 'u',
            "u": 'u',
            "default": 'd',
            "def": 'd',
            "d": 'd',
        }
        sub_key = key_map.get(radix.lower())
        if not sub_key:
            raise ValueError(f"Unknown radix '{radix}'")

        popup = self.struct_open_row_context_menu(row_index=row_index)
        _post_message_w(popup, win32con.WM_CHAR, ord('a'), 0)
        time.sleep(0.15)
        _post_message_w(popup, win32con.WM_CHAR, ord(sub_key), 0)
        time.sleep(0.3)

    def struct_set_radix_item(self, row_index: int, radix: str):
        """Set struct list radix for INDIVIDUAL field via context menu (B -> S/U/H/D).
        radix can be 'hex' ('h'), 'dec_signed' ('s'), 'dec_unsigned' ('u'), 'default' ('d')."""
        key_map = {
            "hex": 'h',
            "h": 'h',
            "dec_signed": 's',
            "dec": 's',
            "s": 's',
            "dec_unsigned": 'u',
            "u": 'u',
            "default": 'd',
            "def": 'd',
            "d": 'd',
        }
        sub_key = key_map.get(radix.lower())
        if not sub_key:
            raise ValueError(f"Unknown radix '{radix}'")

        popup = self.struct_open_row_context_menu(row_index=row_index)
        _post_message_w(popup, win32con.WM_CHAR, ord('b'), 0)
        time.sleep(0.15)
        _post_message_w(popup, win32con.WM_CHAR, ord(sub_key), 0)
        time.sleep(0.3)

    def get_statusbar_pane_text(self, part_index: int = 0) -> str:
        """Get text from status bar pane (default part 0 = Address / Status)."""
        info = self.get_statusbar_info()
        if not info:
            return ""
        sb_h, _, _ = info
        return _statusbar_part_text(self.app.window(handle=sb_h), part_index)

    def get_all_statusbar_text(self) -> list[str]:
        """Get text from all status bar panes in the control's native format."""
        info = self.get_statusbar_info()
        if not info:
            return []
        sb_h, _, part_count = info
        status_bar = self.app.window(handle=sb_h)
        return [_statusbar_part_text(status_bar, index) for index in range(part_count)]

    def find_popup_menu(self, timeout: float = 1.0) -> int | None:
        """Find the active Win32 popup menu window (class '#32768')."""
        start_time = time.time()
        while time.time() - start_time < timeout:
            popup = win32gui.FindWindow("#32768", None)
            if popup and win32gui.IsWindow(popup) and win32gui.IsWindowVisible(popup):
                return popup
            time.sleep(0.05)
        return None

    # ------------------------------------------------------------------
    # Help validation: mismatch / compare / sync / print preview
    # ------------------------------------------------------------------
    def find_process_dialog(self, title: str, timeout: float = 3.0) -> int:
        """Find a visible process-owned #32770 dialog by exact title."""
        def _find():
            for hwnd, cls, caption in self._get_process_windows():
                if cls == "#32770" and caption == title:
                    return hwnd
            raise RuntimeError(f"Dialog not found: {title}")

        return timings.wait_until_passes(timeout, 0.1, _find)

    @staticmethod
    def click_dialog_button(dialog_hwnd: int, control_id: int):
        button = win32gui.GetDlgItem(dialog_hwnd, control_id)
        if not button:
            raise RuntimeError(f"Dialog control not found: {control_id}")
        win32gui.PostMessage(button, win32con.BM_CLICK, 0, 0)
        time.sleep(0.2)

    def open_find_mismatch_dialog(self) -> int:
        self.post_command(ID_FIND_MISMATCH)
        return self.find_process_dialog("不一致検索")

    def configure_find_mismatch(
        self, dialog_hwnd: int, value: str, range_mode: str = "cursor"
    ):
        edit = win32gui.GetDlgItem(dialog_hwnd, IDC_MISMATCH_BYTE)
        _set_control_text(edit, value)
        ids = {
            "cursor": IDC_MISMATCH_RANGE_CURSOR,
            "all": IDC_MISMATCH_RANGE_ALL,
            "selection": IDC_MISMATCH_RANGE_SEL,
        }
        control_id = ids[range_mode]
        self.click_dialog_button(dialog_hwnd, control_id)

    def execute_find_mismatch(self, dialog_hwnd: int, forward: bool = True):
        self.click_dialog_button(
            dialog_hwnd, IDC_MISMATCH_NEXT if forward else IDC_MISMATCH_PREV
        )

    def open_compare_dialog(self) -> int:
        self.post_command(ID_COMPARE)
        return self.find_process_dialog("比較")

    def compare_candidates(self, dialog_hwnd: int) -> list[str]:
        return _listbox_texts(win32gui.GetDlgItem(dialog_hwnd, IDC_COMPARE_LIST))

    def accept_compare(self, dialog_hwnd: int, index: int = 0):
        listbox = win32gui.GetDlgItem(dialog_hwnd, IDC_COMPARE_LIST)
        win32gui.SendMessage(listbox, win32con.LB_SETCURSEL, index, 0)
        self.click_dialog_button(dialog_hwnd, win32con.IDOK)

    def find_diff_list_dialog(self, timeout: float = 3.0) -> int:
        title = "相違箇所一覧"

        def _find():
            # The real diff list is an owner-owned top-level popup.  The
            # minimized proxy has the same caption but is an MDI child, so
            # enumerate process top-level windows here rather than descendants.
            for hwnd, cls, caption in self._get_process_windows():
                if cls == "#32770" and caption == title:
                    return hwnd
            raise RuntimeError(f"Diff list dialog not found: {title}")

        return timings.wait_until_passes(timeout, 0.1, _find)

    def find_diff_list_proxy(self, timeout: float = 3.0) -> int:
        """Find the visible MDI-client child used while the diff list is hidden."""
        title = "相違箇所一覧"

        def _find():
            mdi = self.get_mdi_client()
            if not mdi:
                raise RuntimeError("MDI client not found")
            matches: list[int] = []

            def _enum(hwnd, _):
                _, pid = win32process.GetWindowThreadProcessId(hwnd)
                if (pid == self.pid and win32gui.GetParent(hwnd) == mdi
                        and win32gui.IsWindowVisible(hwnd)
                        and _control_text(hwnd) == title):
                    matches.append(hwnd)
                return True

            win32gui.EnumChildWindows(mdi, _enum, None)
            if matches:
                return matches[0]
            raise RuntimeError(f"Diff list proxy not found: {title}")

        return timings.wait_until_passes(timeout, 0.1, _find)

    def diff_list_window_info(self, dialog_hwnd: int) -> dict:
        """Return geometry/style state for the real diff list popup."""
        mdi = self.get_mdi_client()
        if not mdi:
            raise RuntimeError("MDI client not found")
        style = win32gui.GetWindowLong(dialog_hwnd, win32con.GWL_STYLE) & 0xFFFFFFFF
        exstyle = win32gui.GetWindowLong(dialog_hwnd, win32con.GWL_EXSTYLE) & 0xFFFFFFFF
        return {
            "parent": win32gui.GetParent(dialog_hwnd),
            "owner": win32gui.GetWindow(dialog_hwnd, win32con.GW_OWNER),
            "style": style,
            "exstyle": exstyle,
            "iconic": bool(win32gui.IsIconic(dialog_hwnd)),
            "rect": win32gui.GetWindowRect(dialog_hwnd),
            "mdi": mdi,
            "mdi_rect": win32gui.GetWindowRect(mdi),
            "mdi_client_rect": win32gui.GetClientRect(mdi),
        }

    @staticmethod
    def diff_list_proxy_window_info(proxy_hwnd: int) -> dict:
        """Return geometry/style state for the minimized MDI proxy."""
        style = win32gui.GetWindowLong(proxy_hwnd, win32con.GWL_STYLE) & 0xFFFFFFFF
        exstyle = win32gui.GetWindowLong(proxy_hwnd, win32con.GWL_EXSTYLE) & 0xFFFFFFFF
        return {
            "parent": win32gui.GetParent(proxy_hwnd),
            "owner": win32gui.GetWindow(proxy_hwnd, win32con.GW_OWNER),
            "style": style,
            "exstyle": exstyle,
            "iconic": bool(win32gui.IsIconic(proxy_hwnd)),
            "rect": win32gui.GetWindowRect(proxy_hwnd),
        }

    @staticmethod
    def diff_list_is_uncovered(dialog_hwnd: int) -> bool:
        """Return whether the dialog's caption and client center are topmost."""
        if not win32gui.IsWindow(dialog_hwnd) or not win32gui.IsWindowVisible(dialog_hwnd):
            return False
        left, top, right, bottom = win32gui.GetWindowRect(dialog_hwnd)
        if right <= left or bottom <= top:
            return False
        points = [
            ((left + right) // 2, top + 2),
            ((left + right) // 2, (top + bottom) // 2),
        ]
        for point in points:
            hit = win32gui.WindowFromPoint(point)
            if hit != dialog_hwnd and not win32gui.IsChild(dialog_hwnd, hit):
                return False
        return True

    def minimize_diff_list(self, dialog_hwnd: int) -> int:
        # The real dialog deliberately consumes SC_MINIMIZE and hides itself;
        # the proxy is then created under the MDI client.
        win32gui.SendMessage(dialog_hwnd, win32con.WM_SYSCOMMAND,
                             win32con.SC_MINIMIZE, 0)

        def _check():
            if not win32gui.IsWindowVisible(dialog_hwnd):
                return self.find_diff_list_proxy(timeout=0.1)
            raise RuntimeError("diff list did not hide")

        return timings.wait_until_passes(3.0, 0.05, _check)

    def restore_diff_list(self, dialog_hwnd: int) -> None:
        proxy = self.find_diff_list_proxy()
        win32gui.SendMessage(proxy, win32con.WM_SYSCOMMAND,
                             win32con.SC_RESTORE, 0)

        def _check():
            if (win32gui.IsWindowVisible(dialog_hwnd)
                    and not self._diff_list_proxy_exists()):
                return True
            raise RuntimeError("diff list did not restore from proxy")

        timings.wait_until_passes(3.0, 0.05, _check)

    def _diff_list_proxy_exists(self) -> bool:
        try:
            self.find_diff_list_proxy(timeout=0.05)
            return True
        except Exception:
            return False

    def get_diff_list_rows(self, dialog_hwnd: int) -> list[tuple[str, str, str]]:
        list_hwnd = win32gui.GetDlgItem(dialog_hwnd, IDC_DIFFLIST_LIST)
        list_view = self.app.window(handle=list_hwnd).wrapper_object()
        item_count = _send_message_w(list_hwnd, LVM_GETITEMCOUNT)
        return [
            tuple(
                _listview_item_text(list_view, row, column)
                for column in range(3)
            )
            for row in range(item_count)
        ]

    def diff_list_checks(self, dialog_hwnd: int) -> tuple[bool, bool]:
        return tuple(
            win32gui.SendMessage(win32gui.GetDlgItem(dialog_hwnd, control_id),
                                 win32con.BM_GETCHECK, 0, 0)
            == win32con.BST_CHECKED
            for control_id in (IDC_DIFFLIST_HILITE, IDC_DIFFLIST_SYNC)
        )

    def get_mdi_views(self) -> list[tuple[str, int, int]]:
        """Return visible MDI documents as (title, child hwnd, view hwnd)."""
        mdi_clients: list[int] = []

        def _find_mdi(hwnd, _):
            if win32gui.GetClassName(hwnd) == "MDIClient":
                mdi_clients.append(hwnd)
            return True

        win32gui.EnumChildWindows(self.hwnd, _find_mdi, None)
        if not mdi_clients:
            return []
        children: list[int] = []

        def _find_child(hwnd, _):
            if _is_mdi_document_child(hwnd, mdi_clients[0]):
                children.append(hwnd)
            return True

        win32gui.EnumChildWindows(mdi_clients[0], _find_child, None)
        result = []
        for child in children:
            views: list[int] = []

            def _find_view(hwnd, _):
                if win32gui.GetParent(hwnd) == child:
                    views.append(hwnd)
                return True

            win32gui.EnumChildWindows(child, _find_view, None)
            if views:
                result.append((_control_text(child), child, views[0]))
        return result

    def active_mdi_title(self) -> str:
        # EnumChildWindows does not return handles; query the MDI client explicitly.
        mdi_clients: list[int] = []

        def _find_mdi(hwnd, _):
            if win32gui.GetClassName(hwnd) == "MDIClient":
                mdi_clients.append(hwnd)
            return True

        win32gui.EnumChildWindows(self.hwnd, _find_mdi, None)
        if not mdi_clients:
            return ""
        active = win32gui.SendMessage(mdi_clients[0], 0x0229, 0, 0)  # WM_MDIGETACTIVE
        return _control_text(active) if active else ""

    def open_sync_scroll_dialog(self) -> int:
        self.post_command(ID_SYNC_SCROLL)
        return self.find_process_dialog("シンクロスクロール")

    def sync_scroll_lists(self, dialog_hwnd: int) -> tuple[list[str], list[str]]:
        candidates = _listbox_texts(
            win32gui.GetDlgItem(dialog_hwnd, IDC_SYNC_CANDIDATE)
        )
        registered = _listbox_texts(
            win32gui.GetDlgItem(dialog_hwnd, IDC_SYNC_REGISTERED)
        )
        return candidates, registered

    def open_print_range_dialog(self) -> tuple[int, int]:
        self.post_command(ID_PRINT_RANGE)
        dialog = self.find_process_dialog("範囲を指定して印刷")
        range_bars: list[int] = []

        def _find_range_bar(hwnd, _):
            if (win32gui.GetClassName(hwnd) == "#32770"
                    and win32gui.GetDlgItem(hwnd, IDC_RANGEBAR_START)
                    and win32gui.GetDlgItem(hwnd, IDC_RANGEBAR_END)):
                range_bars.append(hwnd)
            return True

        win32gui.EnumChildWindows(dialog, _find_range_bar, None)
        if not range_bars:
            raise RuntimeError("Print range bar not found")
        return dialog, range_bars[0]

    def set_print_range(
        self,
        dialog_hwnd: int,
        range_bar_hwnd: int,
        start: str,
        end: str,
        is_hex: bool = True,
        preview: bool = True,
    ):
        self.click_dialog_button(
            range_bar_hwnd,
            IDC_RANGEBAR_BASE_HEX if is_hex else IDC_RANGEBAR_BASE_DEC,
        )
        _set_control_text(win32gui.GetDlgItem(range_bar_hwnd, IDC_RANGEBAR_START), start)
        _set_control_text(win32gui.GetDlgItem(range_bar_hwnd, IDC_RANGEBAR_END), end)
        check = win32gui.GetDlgItem(dialog_hwnd, IDC_PRINTRANGE_PREVIEW)
        current = win32gui.SendMessage(check, win32con.BM_GETCHECK, 0, 0)
        desired = win32con.BST_CHECKED if preview else win32con.BST_UNCHECKED
        if current != desired:
            self.click_dialog_button(dialog_hwnd, IDC_PRINTRANGE_PREVIEW)

    def is_print_preview_active(self) -> bool:
        close_buttons: list[int] = []

        def _find(hwnd, _):
            if (win32gui.GetDlgCtrlID(hwnd) == AFX_ID_PREVIEW_CLOSE
                    and win32gui.IsWindowVisible(hwnd)):
                close_buttons.append(hwnd)
            return True

        win32gui.EnumChildWindows(self.hwnd, _find, None)
        return bool(close_buttons)

    def close_print_preview(self):
        close_buttons: list[int] = []

        def _find(hwnd, _):
            if win32gui.GetDlgCtrlID(hwnd) == AFX_ID_PREVIEW_CLOSE:
                close_buttons.append(hwnd)
            return True

        win32gui.EnumChildWindows(self.hwnd, _find, None)
        if close_buttons:
            win32gui.PostMessage(close_buttons[0], win32con.BM_CLICK, 0, 0)
        else:
            self.post_command(AFX_ID_PREVIEW_CLOSE)
        time.sleep(0.5)

    def dismiss_popup_menu(self):
        """Dismiss active popup menu by sending ESCAPE key."""
        popup = self.find_popup_menu(timeout=0.5)
        if popup:
            win32gui.PostMessage(popup, win32con.WM_KEYDOWN, win32con.VK_ESCAPE, 0)
            win32gui.PostMessage(popup, win32con.WM_KEYUP, win32con.VK_ESCAPE, 0)
            time.sleep(0.2)

    def select_popup_menu_item(self, down_count: int = 0):
        """Select a popup menu item using Down arrows and Enter."""
        popup = self.find_popup_menu(timeout=1.0)
        if not popup:
            raise RuntimeError("Popup menu (#32768) not found")
        for _ in range(down_count):
            win32gui.PostMessage(popup, win32con.WM_KEYDOWN, win32con.VK_DOWN, 0)
            win32gui.PostMessage(popup, win32con.WM_KEYUP, win32con.VK_DOWN, 0)
            time.sleep(0.05)
        win32gui.PostMessage(popup, win32con.WM_KEYDOWN, win32con.VK_RETURN, 0)
        win32gui.PostMessage(popup, win32con.WM_KEYUP, win32con.VK_RETURN, 0)
        time.sleep(0.3)

    def invoke_context_menu(self):
        """Invoke context menu on active view via WM_CONTEXTMENU.

        The main frame - not the view - must be brought to the foreground.
        SetForegroundWindow() on the child view makes a child window the foreground
        window, and TrackPopupMenu() then refuses to show the menu (its owner is no
        longer the foreground window), so the popup never appears.
        """
        safe_set_focus(self.hwnd)
        view_hwnd = self.get_view_hwnd()
        # Post WM_CONTEXTMENU with lParam = -1 (keyboard context menu)
        win32gui.PostMessage(view_hwnd, win32con.WM_CONTEXTMENU, view_hwnd, -1)
        time.sleep(0.3)

    def open_env_settings_dialog(self, timeout: float = 2.0) -> int:
        """Open Environment Settings dialog (0x8050 / ID_SETTINGS_ENV)."""
        self.post_command(ID_SETTINGS_ENV)
        start_time = time.time()
        while time.time() - start_time < timeout:
            for h, cls, title in self._get_process_windows():
                if "環境設定" in title or "Settings" in title:
                    return h
            time.sleep(0.1)
        raise RuntimeError("Environment Settings dialog not found")

    def toggle_bit_image(self):
        """Toggle Bit Image window (33003 / ID_BITIMAGE)."""
        self.post_command(ID_BITIMAGE)
        time.sleep(0.4)

    def reload_bit_image(self):
        """Reload / Update Bit Image window (33004 / ID_BITIMAGE_RELOAD)."""
        self.post_command(ID_BITIMAGE_RELOAD)
        time.sleep(0.3)

    def find_bit_image_window(self, timeout: float = 2.0) -> int | None:
        """Find the floating top-level Bit Image window."""
        start_time = time.time()
        while time.time() - start_time < timeout:
            for h, cls, title in self._get_process_windows():
                if any(k in title for k in ["ビットイメージ", "BitImage", "Bit Image"]):
                    if win32gui.IsWindow(h) and win32gui.IsWindowVisible(h):
                        return h
            time.sleep(0.1)
        return None

    def find_output_bar(self) -> int | None:
        """Find the output control bar by its control id.

        The bar is reparented as it docks: under a CDockBar of the main frame while
        docked, and into a floating mini frame - a top-level window of the process,
        not a child of the frame - once dragged out. Walk every top-level window of
        the process so both states are found.
        """
        found: list[int] = []

        def _enum_child(h, _):
            try:
                if (win32gui.GetDlgCtrlID(h) in OUTPUT_BAR_IDS
                        and _control_text(h) == OUTPUT_BAR_CAPTION):
                    found.append(h)
            except Exception:
                pass
            return True

        def _enum_top(h, _):
            try:
                _, p = win32process.GetWindowThreadProcessId(h)
                if p == self.pid:
                    win32gui.EnumChildWindows(h, _enum_child, None)
            except Exception:
                pass
            return True

        try:
            win32gui.EnumWindows(_enum_top, None)
        except Exception:
            pass
        return found[0] if found else None

    def find_bit_image_bar(self) -> int | None:
        """Find the bit image control bar of the port by control id and caption.

        Like the output bar it is reparented as it docks - under a CDockBar of the main
        frame while docked, into a floating mini frame once dragged out - so every
        top-level window of the process is walked.
        """
        found: list[int] = []

        def _enum_child(h, _):
            try:
                if (win32gui.GetDlgCtrlID(h) == IDW_BITIMAGE_BAR
                        and _control_text(h) == BITIMAGE_BAR_CAPTION):
                    found.append(h)
            except Exception:
                pass
            return True

        def _enum_top(h, _):
            try:
                _, p = win32process.GetWindowThreadProcessId(h)
                if p == self.pid:
                    win32gui.EnumChildWindows(h, _enum_child, None)
            except Exception:
                pass
            return True

        try:
            win32gui.EnumWindows(_enum_top, None)
        except Exception:
            pass
        return found[0] if found else None

    def _require_bit_image_bar(self) -> int:
        bar = self.find_bit_image_bar()
        if not bar:
            raise AssertionError(
                "bit image control bar (id 0x%04X, caption %r) not found"
                % (IDW_BITIMAGE_BAR, BITIMAGE_BAR_CAPTION)
            )
        return bar

    def is_bit_image_visible(self) -> bool:
        """Whether the bit image pane is currently shown.

        Raises when the bar cannot be found at all, so a caller asserting on a hidden
        pane cannot pass just because the lookup broke.
        """
        return bool(win32gui.IsWindowVisible(self._require_bit_image_bar()))

    def is_bit_image_floating(self) -> bool:
        """Whether the bit image bar sits in a floating mini frame rather than a dock bar."""
        bar = self._require_bit_image_bar()
        return not _has_mdi_client(_top_level_of(bar))

    def bit_image_frame_hwnd(self) -> int:
        """Top-level window holding the bar: the mini frame while floating."""
        return _top_level_of(self._require_bit_image_bar())

    def bit_image_frame_rect(self) -> tuple[int, int, int, int]:
        """Screen rect of the floating mini frame (raises when the bar is docked)."""
        if not self.is_bit_image_floating():
            raise AssertionError("bit image bar is docked, it has no floating frame")
        return win32gui.GetWindowRect(self.bit_image_frame_hwnd())

    def move_bit_image_window(self, x: int, y: int, timeout: float = 2.0):
        """Move the floating bit image window to a screen position and wait for it to land."""
        frame = self.bit_image_frame_hwnd()
        win32gui.SetWindowPos(frame, 0, x, y, 0, 0,
                              win32con.SWP_NOZORDER | win32con.SWP_NOSIZE
                              | win32con.SWP_NOACTIVATE)
        deadline = time.time() + timeout
        while time.time() < deadline:
            rect = win32gui.GetWindowRect(frame)
            if rect[0] == x and rect[1] == y:
                return
            time.sleep(0.05)
        raise AssertionError(
            "bit image window did not move to (%d, %d): %s"
            % (x, y, win32gui.GetWindowRect(frame))
        )

    def is_output_pane_visible(self) -> bool:
        """Whether the output pane is currently shown.

        Raises when the bar cannot be found at all, so a caller asserting on a hidden
        pane cannot pass just because the lookup broke.
        """
        bar = self.find_output_bar()
        if not bar:
            raise AssertionError(
                "output control bar (id %s, caption %r) not found"
                % (", ".join("0x%04X" % i for i in OUTPUT_BAR_IDS), OUTPUT_BAR_CAPTION)
            )
        return bool(win32gui.IsWindowVisible(bar))

    def toggle_output_pane(self):
        """Toggle the output pane (0x80E8)."""
        self.post_command(ID_OUTPUT_PANE)
        time.sleep(0.3)

    # ------------------------------------------------------------------
    # Environment Settings "User Menu" page / accelerator dialog (Issue #27)
    # ------------------------------------------------------------------
    def _find_settings_page(self, sheet_hwnd: int, probe_ctrl_ids: list[int]) -> int | None:
        """Find the visible property page (#32770) of a sheet that owns all probe_ctrl_ids."""
        found: list[int] = []

        def _enum(h, _):
            try:
                if win32gui.GetClassName(h) == "#32770" and win32gui.IsWindowVisible(h):
                    if all(win32gui.GetDlgItem(h, i) for i in probe_ctrl_ids):
                        found.append(h)
            except Exception:
                pass
            return True

        win32gui.EnumChildWindows(sheet_hwnd, _enum, None)
        return found[0] if found else None

    def _find_tab_control(self, sheet_hwnd: int) -> int | None:
        """Find the property sheet's tab control."""
        found: list[int] = []

        def _enum(h, _):
            try:
                if win32gui.GetClassName(h) == "SysTabControl32":
                    found.append(h)
            except Exception:
                pass
            return True

        win32gui.EnumChildWindows(sheet_hwnd, _enum, None)
        return found[0] if found else None

    def open_user_menu_page(self, timeout: float = 5.0) -> tuple[int, int]:
        """Open Environment Settings and switch to the "ユーザーメニュー" page.

        The page is located by its controls, not by the tab caption: reading tab texts of
        the 32-bit original from 64-bit Python is unreliable, and the page order differs
        between the original and the port. Returns (sheet_hwnd, page_hwnd).
        """
        sheet_hwnd = self.open_env_settings_dialog(timeout=timeout)
        probe = [IDC_UM_MENUSET, IDC_UM_CURRENT, IDC_UM_AVAILABLE]

        page = self._find_settings_page(sheet_hwnd, probe)
        if page is not None:
            return sheet_hwnd, page

        tab = self._find_tab_control(sheet_hwnd)
        if tab is None:
            raise RuntimeError("Property sheet tab control not found")
        count = win32gui.SendMessage(tab, TCM_GETITEMCOUNT, 0, 0) or 12
        for index in range(count):
            win32gui.SendMessage(tab, TCM_SETCURFOCUS, index, 0)
            time.sleep(0.2)
            page = self._find_settings_page(sheet_hwnd, probe)
            if page is not None:
                return sheet_hwnd, page
        raise RuntimeError("User Menu page not found in Environment Settings")

    def open_edit1_page(self, timeout: float = 5.0) -> tuple[int, int]:
        """Open Environment Settings and switch to the "編集１" page.

        Probed by controls the original has too, and by a combination that page 編集２
        does not share. Returns (sheet_hwnd, page_hwnd).
        """
        sheet_hwnd = self.open_env_settings_dialog(timeout=timeout)
        probe = [IDC_ED1_CLEAR_UNDO_ON_SAVE, IDC_ED1_HILIGHT_BOTH,
                 IDC_ED1_REALTIME_BITIMAGE]
        page = self._find_settings_page(sheet_hwnd, probe)
        if page is not None:
            return sheet_hwnd, page

        tab = self._find_tab_control(sheet_hwnd)
        if tab is None:
            raise RuntimeError("Property sheet tab control not found")
        count = win32gui.SendMessage(tab, TCM_GETITEMCOUNT, 0, 0) or 12
        for index in range(count):
            win32gui.SendMessage(tab, TCM_SETCURFOCUS, index, 0)
            time.sleep(0.2)
            page = self._find_settings_page(sheet_hwnd, probe)
            if page is not None:
                return sheet_hwnd, page
        raise RuntimeError("Edit 1 page not found in Environment Settings")

    def open_edit2_page(self, timeout: float = 5.0) -> tuple[int, int]:
        """Open Environment Settings and switch to the "編集２" page.

        Probed by controls that page 編集１ does not share.  Returns
        (sheet_hwnd, page_hwnd).
        """
        sheet_hwnd = self.open_env_settings_dialog(timeout=timeout)
        probe = [IDC_ED2_CARET_RESTORE, IDC_ED2_DYNAMIC_MARK,
                 IDC_ED2_MARK_AUTO_RESTORE]
        page = self._find_settings_page(sheet_hwnd, probe)
        if page is not None:
            return sheet_hwnd, page

        tab = self._find_tab_control(sheet_hwnd)
        if tab is None:
            raise RuntimeError("Property sheet tab control not found")
        count = win32gui.SendMessage(tab, TCM_GETITEMCOUNT, 0, 0) or 12
        for index in range(count):
            win32gui.SendMessage(tab, TCM_SETCURFOCUS, index, 0)
            time.sleep(0.2)
            page = self._find_settings_page(sheet_hwnd, probe)
            if page is not None:
                return sheet_hwnd, page
        raise RuntimeError("Edit 2 page not found in Environment Settings")

    def open_file_page(self, timeout: float = 5.0) -> tuple[int, int]:
        """Open Environment Settings and switch to the "ファイル" page.

        Probed by controls the original has too, so the page is found the same way in
        both builds. Returns (sheet_hwnd, page_hwnd).
        """
        sheet_hwnd = self.open_env_settings_dialog(timeout=timeout)
        probe = [IDC_FILE_BACKUP_CREATE, IDC_FILE_BACKUP_FOLDER_CHK, IDC_FILE_EXCL_NONE]
        page = self._find_settings_page(sheet_hwnd, probe)
        if page is not None:
            return sheet_hwnd, page

        tab = self._find_tab_control(sheet_hwnd)
        if tab is None:
            raise RuntimeError("Property sheet tab control not found")
        count = win32gui.SendMessage(tab, TCM_GETITEMCOUNT, 0, 0) or 12
        for index in range(count):
            win32gui.SendMessage(tab, TCM_SETCURFOCUS, index, 0)
            time.sleep(0.2)
            page = self._find_settings_page(sheet_hwnd, probe)
            if page is not None:
                return sheet_hwnd, page
        raise RuntimeError("File page not found in Environment Settings")

    def read_control_text(self, page_hwnd: int, ctrl_id: int) -> str:
        """Read one control's text from a dialog page."""
        h = win32gui.GetDlgItem(page_hwnd, ctrl_id)
        if not h:
            raise RuntimeError(f"control {ctrl_id} not found")
        return _control_text(h)

    def open_key_assign_page(self, timeout: float = 5.0) -> tuple[int, int]:
        """Open Environment Settings and switch to the Key Assign page."""
        sheet_hwnd = self.open_env_settings_dialog(timeout=timeout)
        probe = [IDC_KA_KEYLIST, IDC_KA_CTRL, IDC_KA_SHIFT,
                 IDC_KA_FUNC_LIST, IDC_KA_FUNC_CATEGORY]
        page = self._find_settings_page(sheet_hwnd, probe)
        if page is not None:
            return sheet_hwnd, page

        tab = self._find_tab_control(sheet_hwnd)
        if tab is None:
            raise RuntimeError("Property sheet tab control not found")
        count = win32gui.SendMessage(tab, TCM_GETITEMCOUNT, 0, 0) or 12
        for index in range(count):
            win32gui.SendMessage(tab, TCM_SETCURFOCUS, index, 0)
            time.sleep(0.2)
            page = self._find_settings_page(sheet_hwnd, probe)
            if page is not None:
                return sheet_hwnd, page
        raise RuntimeError("Key Assign page not found in Environment Settings")

    def open_toolbar_page(self, timeout: float = 5.0) -> tuple[int, int]:
        """Open Environment Settings and switch to the Toolbar page."""
        sheet_hwnd = self.open_env_settings_dialog(timeout=timeout)
        probe = [IDC_TBAR_CURRENT, IDC_TBAR_CATEGORY, IDC_TBAR_AVAILABLE,
                 IDC_TBAR_SEPARATOR]
        page = self._find_settings_page(sheet_hwnd, probe)
        if page is not None:
            return sheet_hwnd, page

        tab = self._find_tab_control(sheet_hwnd)
        if tab is None:
            raise RuntimeError("Property sheet tab control not found")
        count = win32gui.SendMessage(tab, TCM_GETITEMCOUNT, 0, 0) or 12
        for index in range(count):
            win32gui.SendMessage(tab, TCM_SETCURFOCUS, index, 0)
            time.sleep(0.2)
            page = self._find_settings_page(sheet_hwnd, probe)
            if page is not None:
                return sheet_hwnd, page
        raise RuntimeError("Toolbar page not found in Environment Settings")

    def toolbar_items(self, page_hwnd: int, *, current: bool) -> list[int]:
        control_id = IDC_TBAR_CURRENT if current else IDC_TBAR_AVAILABLE
        return _listbox_item_data(win32gui.GetDlgItem(page_hwnd, control_id))

    def toolbar_categories(self, page_hwnd: int) -> list[str]:
        return _combobox_texts(win32gui.GetDlgItem(page_hwnd, IDC_TBAR_CATEGORY))

    def toolbar_select_category(self, page_hwnd: int, index: int):
        combo = win32gui.GetDlgItem(page_hwnd, IDC_TBAR_CATEGORY)
        win32gui.SendMessage(combo, win32con.CB_SETCURSEL, index, 0)
        win32gui.SendMessage(
            page_hwnd, win32con.WM_COMMAND,
            (win32con.CBN_SELCHANGE << 16) | IDC_TBAR_CATEGORY, combo,
        )
        time.sleep(0.1)

    def toolbar_select_item(self, page_hwnd: int, index: int, *, current: bool):
        control_id = IDC_TBAR_CURRENT if current else IDC_TBAR_AVAILABLE
        listbox = win32gui.GetDlgItem(page_hwnd, control_id)
        win32gui.SendMessage(listbox, win32con.LB_SETCURSEL, index, 0)
        win32gui.SendMessage(
            page_hwnd, win32con.WM_COMMAND,
            (win32con.LBN_SELCHANGE << 16) | control_id, listbox,
        )
        time.sleep(0.1)

    def toolbar_click(self, page_hwnd: int, control_id: int):
        button = win32gui.GetDlgItem(page_hwnd, control_id)
        if not button:
            raise RuntimeError(f"Toolbar page control not found: {control_id}")
        win32gui.SendMessage(page_hwnd, win32con.WM_COMMAND, control_id, button)
        time.sleep(0.1)

    def toolbar_button_states(self, page_hwnd: int) -> dict[int, bool]:
        return {
            control_id: bool(win32gui.IsWindowEnabled(
                win32gui.GetDlgItem(page_hwnd, control_id)
            ))
            for control_id in (
                IDC_TBAR_ADD, IDC_TBAR_DELETE, IDC_TBAR_UP, IDC_TBAR_DOWN,
                IDC_TBAR_SEPARATOR,
            )
        }

    def toolbar_button_count(self) -> int:
        toolbars: list[int] = []

        def _find(hwnd, _):
            if win32gui.GetClassName(hwnd) == "ToolbarWindow32":
                toolbars.append(hwnd)
            return True

        win32gui.EnumChildWindows(self.hwnd, _find, None)
        if not toolbars:
            raise RuntimeError("Main toolbar not found")
        return int(win32gui.SendMessage(toolbars[0], 0x0418, 0, 0))  # TB_BUTTONCOUNT

    def open_extension_settings_dialog(self, timeout: float = 5.0) -> int:
        self.post_command(ID_SETTINGS_EXT)

        def _find():
            for hwnd, cls, _title in self._get_process_windows():
                if (cls == "#32770"
                        and _safe_dlg_item(hwnd, IDC_EXTLIST_LIST)
                        and _safe_dlg_item(hwnd, IDC_EXTLIST_SETTINGS)
                        and _safe_dlg_item(hwnd, IDC_EXTLIST_ADD)):
                    return hwnd
            raise RuntimeError("Extension Settings list dialog not found")

        return timings.wait_until_passes(timeout, 0.1, _find)

    def extension_list_rows(self, dialog_hwnd: int) -> list[str]:
        return _listbox_texts(win32gui.GetDlgItem(dialog_hwnd, IDC_EXTLIST_LIST))

    def extension_select(self, dialog_hwnd: int, index: int):
        listbox = win32gui.GetDlgItem(dialog_hwnd, IDC_EXTLIST_LIST)
        win32gui.SendMessage(listbox, win32con.LB_SETCURSEL, index, 0)
        win32gui.PostMessage(
            dialog_hwnd, win32con.WM_COMMAND,
            (win32con.LBN_SELCHANGE << 16) | IDC_EXTLIST_LIST, listbox,
        )
        time.sleep(0.1)

    def extension_open_record(
        self, dialog_hwnd: int, *, add: bool = False, double_click: bool = False
    ) -> tuple[int, int]:
        if double_click:
            listbox = win32gui.GetDlgItem(dialog_hwnd, IDC_EXTLIST_LIST)
            win32gui.PostMessage(
                dialog_hwnd, win32con.WM_COMMAND,
                (win32con.LBN_DBLCLK << 16) | IDC_EXTLIST_LIST, listbox,
            )
        else:
            self.click_dialog_button(
                dialog_hwnd, IDC_EXTLIST_ADD if add else IDC_EXTLIST_SETTINGS
            )

        def _find():
            for hwnd, cls, _title in self._get_process_windows():
                if (hwnd != dialog_hwnd and cls == "#32770"
                        and _safe_dlg_item(hwnd, IDC_EXTREC_EXT)
                        and _safe_dlg_item(hwnd, IDC_EXTREC_SHEET)):
                    pages: list[int] = []

                    def _find_page(child, _):
                        if (win32gui.GetClassName(child) == "#32770"
                                and _safe_dlg_item(child, IDC_DISP_LINESIZE)):
                            pages.append(child)
                        return True

                    win32gui.EnumChildWindows(hwnd, _find_page, None)
                    if pages:
                        return hwnd, pages[0]
            raise RuntimeError("Extension record dialog not found")

        return timings.wait_until_passes(5, 0.1, _find)

    def extension_set_header(self, record_hwnd: int, extension: str, comment: str):
        _set_control_text(win32gui.GetDlgItem(record_hwnd, IDC_EXTREC_EXT), extension)
        _set_control_text(win32gui.GetDlgItem(record_hwnd, IDC_EXTREC_COMMENT), comment)

    def extension_configure_display(
        self, page_hwnd: int, *, line_size: int | None = None,
        address_hex: bool | None = None, address_scroll: bool | None = None,
        read_only: bool | None = None, insert: bool | None = None,
        char_mode: bool | None = None, charset: int | None = None,
        byte_order_big: bool | None = None,
    ):
        if line_size is not None:
            _set_control_text(win32gui.GetDlgItem(page_hwnd, IDC_DISP_LINESIZE), str(line_size))
        radio_choices = []
        if address_hex is not None:
            radio_choices.append(IDC_DISP_RADIX_HEX if address_hex else IDC_DISP_RADIX_DEC)
        if charset is not None:
            radio_choices.append([
                IDC_DISP_CS_ASCII, IDC_DISP_CS_SJIS, IDC_DISP_CS_EUC,
                IDC_DISP_CS_UNICODE, IDC_DISP_CS_EBCDIC, IDC_DISP_CS_EBCIDK,
            ][charset])
        if byte_order_big is not None:
            radio_choices.append(IDC_DISP_BO_BIG if byte_order_big else IDC_DISP_BO_LITTLE)
        for control_id in radio_choices:
            self.click_dialog_button(page_hwnd, control_id)
        checks = (
            (IDC_DISP_ADDR_HSCROLL, address_scroll),
            (IDC_DISP_OPEN_READONLY, read_only),
            (IDC_DISP_OPEN_INSERT, insert),
            (IDC_DISP_OPEN_CHARMODE, char_mode),
        )
        for control_id, desired in checks:
            if desired is None:
                continue
            button = win32gui.GetDlgItem(page_hwnd, control_id)
            checked = win32gui.SendMessage(button, win32con.BM_GETCHECK, 0, 0)
            if (checked == win32con.BST_CHECKED) != desired:
                self.click_dialog_button(page_hwnd, control_id)

    def close_dialog(self, dialog_hwnd: int, *, accept: bool, timeout: float = 5.0):
        self.click_dialog_button(dialog_hwnd, win32con.IDOK if accept else win32con.IDCANCEL)

        def _closed():
            if win32gui.IsWindow(dialog_hwnd) and win32gui.IsWindowVisible(dialog_hwnd):
                raise RuntimeError("Dialog still open")
            return True

        timings.wait_until_passes(timeout, 0.1, _closed)
        time.sleep(0.2)

    def key_assign_categories(self, page_hwnd: int) -> list[str]:
        return _combobox_texts(win32gui.GetDlgItem(page_hwnd, IDC_KA_FUNC_CATEGORY))

    def key_assign_keys(self, page_hwnd: int) -> list[str]:
        return _listbox_texts(win32gui.GetDlgItem(page_hwnd, IDC_KA_KEYLIST))

    def key_assign_functions(self, page_hwnd: int) -> list[tuple[str, int]]:
        listbox = win32gui.GetDlgItem(page_hwnd, IDC_KA_FUNC_LIST)
        texts = _listbox_texts(listbox)
        return [
            (text, int(win32gui.SendMessage(listbox, win32con.LB_GETITEMDATA, index, 0)))
            for index, text in enumerate(texts)
        ]

    def key_assign_select_category(self, page_hwnd: int, index: int):
        combo = win32gui.GetDlgItem(page_hwnd, IDC_KA_FUNC_CATEGORY)
        win32gui.SendMessage(combo, win32con.CB_SETCURSEL, index, 0)
        win32gui.SendMessage(
            page_hwnd,
            win32con.WM_COMMAND,
            (win32con.CBN_SELCHANGE << 16) | IDC_KA_FUNC_CATEGORY,
            combo,
        )
        time.sleep(0.1)

    def key_assign_set_modifiers(
        self, page_hwnd: int, *, ctrl: bool = False, shift: bool = False
    ):
        for control_id, desired in ((IDC_KA_CTRL, ctrl), (IDC_KA_SHIFT, shift)):
            button = win32gui.GetDlgItem(page_hwnd, control_id)
            checked = win32gui.SendMessage(button, win32con.BM_GETCHECK, 0, 0)
            if bool(checked == win32con.BST_CHECKED) != desired:
                win32gui.SendMessage(button, win32con.BM_CLICK, 0, 0)
                time.sleep(0.1)

    def key_assign_select_key(self, page_hwnd: int, index: int):
        listbox = win32gui.GetDlgItem(page_hwnd, IDC_KA_KEYLIST)
        win32gui.SendMessage(listbox, win32con.LB_SETCURSEL, index, 0)
        win32gui.SendMessage(
            page_hwnd,
            win32con.WM_COMMAND,
            (win32con.LBN_SELCHANGE << 16) | IDC_KA_KEYLIST,
            listbox,
        )
        time.sleep(0.1)

    def key_assign_select_function(self, page_hwnd: int, raw_id: int):
        category = (raw_id >> 8) & 0xFF
        combo = win32gui.GetDlgItem(page_hwnd, IDC_KA_FUNC_CATEGORY)
        win32gui.SendMessage(combo, win32con.CB_SETCURSEL, category, 0)
        win32gui.SendMessage(
            page_hwnd,
            win32con.WM_COMMAND,
            (win32con.CBN_SELCHANGE << 16) | IDC_KA_FUNC_CATEGORY,
            combo,
        )
        time.sleep(0.1)
        listbox = win32gui.GetDlgItem(page_hwnd, IDC_KA_FUNC_LIST)
        count = win32gui.SendMessage(listbox, win32con.LB_GETCOUNT, 0, 0)
        for index in range(count):
            item = int(win32gui.SendMessage(
                listbox, win32con.LB_GETITEMDATA, index, 0
            ))
            if item == raw_id:
                win32gui.SendMessage(listbox, win32con.LB_SETCURSEL, index, 0)
                win32gui.SendMessage(
                    page_hwnd,
                    win32con.WM_COMMAND,
                    (win32con.LBN_SELCHANGE << 16) | IDC_KA_FUNC_LIST,
                    listbox,
                )
                time.sleep(0.1)
                return
        raise ValueError(f"Key Assign function raw ID not found: 0x{raw_id:04X}")

    def key_assign_current_function(self, page_hwnd: int) -> int:
        listbox = win32gui.GetDlgItem(page_hwnd, IDC_KA_FUNC_LIST)
        index = win32gui.SendMessage(listbox, win32con.LB_GETCURSEL, 0, 0)
        if index == win32con.LB_ERR:
            return -1
        return int(win32gui.SendMessage(
            listbox, win32con.LB_GETITEMDATA, index, 0
        ))

    def key_assign_reset(self, page_hwnd: int):
        button = win32gui.GetDlgItem(page_hwnd, IDC_KA_RESET)
        win32gui.PostMessage(button, win32con.BM_CLICK, 0, 0)
        time.sleep(0.2)

    def key_assign_transfer_file(
        self, page_hwnd: int, path: str | Path, *, save: bool
    ):
        """Drive the Key Assign load/save common dialog using an explicit path."""
        path = str(Path(path).resolve())
        control_id = IDC_KA_SAVE if save else IDC_KA_LOAD
        button = win32gui.GetDlgItem(page_hwnd, control_id)
        sheet = win32gui.GetAncestor(page_hwnd, win32con.GA_ROOT)
        win32gui.PostMessage(button, win32con.BM_CLICK, 0, 0)

        def _find_dialog():
            for hwnd, cls, _title in self._get_process_windows():
                if hwnd != sheet and cls == "#32770":
                    edit = _file_dialog_edit(hwnd)
                    if edit:
                        return hwnd, edit
            raise RuntimeError("Key Assign file dialog not found")

        dialog, edit = timings.wait_until_passes(5, 0.1, _find_dialog)
        _set_control_text(edit, path)
        ok = win32gui.GetDlgItem(dialog, win32con.IDOK)
        win32gui.PostMessage(ok, win32con.BM_CLICK, 0, 0)

        def _closed():
            if win32gui.IsWindow(dialog) and win32gui.IsWindowVisible(dialog):
                raise RuntimeError("Key Assign file dialog still open")
            return True

        timings.wait_until_passes(5, 0.2, _closed)
        time.sleep(0.3)

    def open_mark_list_dialog(self) -> int:
        self.post_command(ID_MARK_LIST)
        return self.find_process_dialog("マーク一覧")

    def mark_list_entries(self, dialog_hwnd: int) -> list[tuple[str, int]]:
        listbox = win32gui.GetDlgItem(dialog_hwnd, IDC_MARKLIST_LIST)
        rows = []
        for index, text in enumerate(_listbox_texts(listbox)):
            item_data = int(win32gui.SendMessage(
                listbox, win32con.LB_GETITEMDATA, index, 0
            ))
            if item_data not in (-1, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF):
                rows.append((text, item_data))
        return rows

    def listbox_texts(self, page_hwnd: int, ctrl_id: int) -> list[str]:
        """Read all item texts of a listbox on a dialog/page."""
        return _listbox_texts(win32gui.GetDlgItem(page_hwnd, ctrl_id))

    def um_select_available(self, page_hwnd: int, index: int = 0):
        """Select an item in the "追加できる機能" listbox and notify the page."""
        lb = win32gui.GetDlgItem(page_hwnd, IDC_UM_AVAILABLE)
        win32gui.SendMessage(lb, win32con.LB_SETCURSEL, index, 0)
        win32gui.SendMessage(page_hwnd, win32con.WM_COMMAND,
                             (1 << 16) | IDC_UM_AVAILABLE, lb)   # LBN_SELCHANGE
        time.sleep(0.1)

    def um_select_current(self, page_hwnd: int, index: int = 0):
        """Select an item in the "現在のメニュー設定" listbox and notify the page."""
        lb = win32gui.GetDlgItem(page_hwnd, IDC_UM_CURRENT)
        win32gui.SendMessage(lb, win32con.LB_SETCURSEL, index, 0)
        win32gui.SendMessage(page_hwnd, win32con.WM_COMMAND,
                             (1 << 16) | IDC_UM_CURRENT, lb)     # LBN_SELCHANGE
        time.sleep(0.1)

    def um_click_add(self, page_hwnd: int):
        """Press the "追加" button (opens the modal accelerator dialog -> must not block)."""
        btn = win32gui.GetDlgItem(page_hwnd, IDC_UM_ADD)
        win32gui.PostMessage(page_hwnd, win32con.WM_COMMAND, IDC_UM_ADD, btn)   # BN_CLICKED
        time.sleep(0.2)

    def um_click_separator(self, page_hwnd: int):
        """Press the "セパレータ" button."""
        btn = win32gui.GetDlgItem(page_hwnd, IDC_UM_SEPARATOR)
        win32gui.SendMessage(page_hwnd, win32con.WM_COMMAND, IDC_UM_SEPARATOR, btn)
        time.sleep(0.2)

    def um_dblclk_current(self, page_hwnd: int, index: int):
        """Double-click an item of the "現在のメニュー設定" listbox (opens the accel dialog)."""
        lb = win32gui.GetDlgItem(page_hwnd, IDC_UM_CURRENT)
        win32gui.SendMessage(lb, win32con.LB_SETCURSEL, index, 0)
        win32gui.PostMessage(page_hwnd, win32con.WM_COMMAND,
                             (2 << 16) | IDC_UM_CURRENT, lb)     # LBN_DBLCLK
        time.sleep(0.2)

    def find_accel_dialog(self, timeout: float = 3.0) -> int | None:
        """Find the modal "アクセラレータの指定" dialog (IDD_ACCEL_INPUT 184)."""
        start_time = time.time()
        while time.time() - start_time < timeout:
            for h, cls, title in self._get_process_windows():
                if cls == "#32770" and ACCEL_DIALOG_TITLE in title and win32gui.IsWindowVisible(h):
                    return h
            time.sleep(0.1)
        return None

    def accel_dialog_text(self, dlg_hwnd: int) -> str:
        """Current content of the accelerator edit box.

        GetWindowTextW() returns "" for a caption-less control in another process,
        so the text has to be pulled with WM_GETTEXT.
        """
        return _control_text(win32gui.GetDlgItem(dlg_hwnd, IDC_ACCEL_EDIT))

    def accel_dialog_ok_enabled(self, dlg_hwnd: int) -> bool:
        """Whether the OK button of the accelerator dialog is enabled."""
        return bool(win32gui.IsWindowEnabled(win32gui.GetDlgItem(dlg_hwnd, 1)))

    def accel_dialog_type(self, dlg_hwnd: int, ch: str):
        """Type one character into the accelerator edit (goes through ES_UPPERCASE)."""
        edit = win32gui.GetDlgItem(dlg_hwnd, IDC_ACCEL_EDIT)
        _send_message_w(edit, win32con.WM_CHAR, ord(ch), 1)
        time.sleep(0.1)

    def accel_dialog_clear(self, dlg_hwnd: int):
        """Clear the accelerator edit (select all + backspace)."""
        edit = win32gui.GetDlgItem(dlg_hwnd, IDC_ACCEL_EDIT)
        win32gui.SendMessage(edit, win32con.EM_SETSEL, 0, -1)
        _send_message_w(edit, win32con.WM_CHAR, 8, 1)   # VK_BACK
        time.sleep(0.1)

    def accel_dialog_close(self, dlg_hwnd: int, accept: bool = True, timeout: float = 5.0):
        """Close the accelerator dialog with OK (accept) or Cancel."""
        win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1 if accept else 2, 0)

        def _check_closed():
            if win32gui.IsWindow(dlg_hwnd) and win32gui.IsWindowVisible(dlg_hwnd):
                raise RuntimeError("Accelerator dialog still open")
            return True

        timings.wait_until_passes(timeout, 0.2, _check_closed)
        time.sleep(0.2)

    def close_settings_sheet(self, sheet_hwnd: int, accept: bool = False, timeout: float = 5.0):
        """Close the Environment Settings property sheet with OK / Cancel.

        The button has to be clicked: a bare WM_COMMAND to the sheet closes the window
        without running the PSN_KILLACTIVE / PSN_APPLY chain, so the pages never commit.
        """
        btn = win32gui.GetDlgItem(sheet_hwnd, 1 if accept else 2)
        win32gui.PostMessage(btn, win32con.BM_CLICK, 0, 0)

        def _check_closed():
            if win32gui.IsWindow(sheet_hwnd) and win32gui.IsWindowVisible(sheet_hwnd):
                raise RuntimeError("Environment Settings sheet still open")
            return True

        timings.wait_until_passes(timeout, 0.2, _check_closed)
        time.sleep(0.2)
