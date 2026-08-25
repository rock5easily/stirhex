import ctypes
import winreg
from ctypes import wintypes
from pathlib import Path
from contextlib import contextmanager
from typing import Any, Dict


# Target registry keys:
# - Original Stirling: HKCU\Software\DDS2\Stirling\Settings
# - StirHex: HKCU\Software\StirHex\StirHex\Env
REG_CONFIGS = [
    {
        "root": r"Software\DDS2\Stirling\Settings",
        "key_map": {
            "dynamic_mark": "DynamicMark",
            "show_sub_caret": "ShowSubCaret",
            "realtime_bit_image": "RealTimeImage",
            "file_exclusive_mode": "Exclusive",
            "open_lnk_direct": "OpenLinkFile",
            "show_status_bar": "ShowStatusBar",
            "auto_set_struct_addr": "AutoSetSEAddress",
            "keep_struct_item_ratio": "SaveItemRatio",
            "struct_bar_pos": "StructEditPlacement",
            "struct_bar_status_pos": "StructEditStatusPlacement",
            "esc_menu": "EscMenu",
            "two_stroke_timeout": "TwoStrokeTimeout",
        }
    },
    {
        "root": r"Software\StirHex\StirHex\Env",
        "key_map": {
            "dynamic_mark": "DynamicMark",
            "show_sub_caret": "SubCaret",
            "realtime_bit_image": "RealtimeBitImage",
            "file_exclusive_mode": "ExclusiveControl",
            "open_lnk_direct": "LinkDirect",
            "show_status_bar": "ShowStatusbar",
            "auto_set_struct_addr": "CurPosToStructAddr",
            "keep_struct_item_ratio": "StructItemRatioKeep",
            "struct_bar_pos": "StructBarPos",
            "struct_bar_status_pos": "StructBarStatusPos",
            "esc_menu": "EscMenu",
            "two_stroke_timeout": "TwoStrokeTimeoutMs",
        }
    },
]


def _read_reg_values(root_key_path: str) -> Dict[str, Any]:
    """Read all values under HKCU\\<root_key_path> into a dictionary."""
    values = {}
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, root_key_path, 0, winreg.KEY_READ) as key:
            index = 0
            while True:
                try:
                    name, val, val_type = winreg.EnumValue(key, index)
                    values[name] = (val, val_type)
                    index += 1
                except OSError:
                    break
    except FileNotFoundError:
        pass
    return values


def _write_reg_values(root_key_path: str, values: Dict[str, Any]):
    """Write values under HKCU\\<root_key_path>, creating key if needed."""
    try:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, root_key_path) as key:
            for name, (val, val_type) in values.items():
                winreg.SetValueEx(key, name, 0, val_type, val)
    except Exception:
        pass


def _delete_reg_values(root_key_path: str, names: list[str]):
    """Delete specified value names under HKCU\\<root_key_path>."""
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, root_key_path, 0, winreg.KEY_SET_VALUE) as key:
            for name in names:
                try:
                    winreg.DeleteValue(key, name)
                except FileNotFoundError:
                    pass
    except FileNotFoundError:
        pass


@contextmanager
def stirling_settings(
    file_exclusive_mode: int | None = None,
    open_lnk_direct: bool | None = None,
    show_sub_caret: bool | None = None,
    realtime_bit_image: bool | None = None,
    dynamic_mark: bool | None = None,
    show_status_bar: bool | None = None,
    auto_set_struct_addr: bool | None = None,
    keep_struct_item_ratio: bool | None = None,
    struct_bar_pos: int | None = None,
    struct_bar_status_pos: int | None = None,
    esc_menu: bool | None = None,
    two_stroke_timeout: int | None = None,
    user_menus: dict[int, list[int]] | None = None,
    **extra_settings
):
    """Context manager that applies prerequisite settings for Stirling before test
    and strictly restores original settings after test (Setup / Teardown).
    
    Supports both Original Stirling (HKCU\\Software\\DDS2\\Stirling\\Settings)
    and StirHex (HKCU\\Software\\StirHex\\StirHex\\Env).
    """
    options = {
        "file_exclusive_mode": file_exclusive_mode,
        "open_lnk_direct": 1 if open_lnk_direct is not None and open_lnk_direct else (0 if open_lnk_direct is not None else None),
        "show_sub_caret": 1 if show_sub_caret is not None and show_sub_caret else (0 if show_sub_caret is not None else None),
        "realtime_bit_image": 1 if realtime_bit_image is not None and realtime_bit_image else (0 if realtime_bit_image is not None else None),
        "dynamic_mark": 1 if dynamic_mark is not None and dynamic_mark else (0 if dynamic_mark is not None else None),
        "show_status_bar": 1 if show_status_bar is not None and show_status_bar else (0 if show_status_bar is not None else None),
        "auto_set_struct_addr": 1 if auto_set_struct_addr is not None and auto_set_struct_addr else (0 if auto_set_struct_addr is not None else None),
        "keep_struct_item_ratio": 1 if keep_struct_item_ratio is not None and keep_struct_item_ratio else (0 if keep_struct_item_ratio is not None else None),
        "struct_bar_pos": struct_bar_pos,
        "struct_bar_status_pos": struct_bar_status_pos,
        "esc_menu": 1 if esc_menu is not None and esc_menu else (0 if esc_menu is not None else None),
        "two_stroke_timeout": two_stroke_timeout,
    }

    backups = {}
    applied_per_root = {}

    for cfg in REG_CONFIGS:
        root = cfg["root"]
        key_map = cfg["key_map"]
        backups[root] = _read_reg_values(root)

        settings_to_apply = {}
        for opt_key, opt_val in options.items():
            if opt_val is not None and opt_key in key_map:
                reg_name = key_map[opt_key]
                settings_to_apply[reg_name] = (opt_val, winreg.REG_DWORD)

        if user_menus:
            settings_to_apply["UserMenuCount"] = (15, winreg.REG_DWORD)
            for m_idx, items in user_menus.items():
                settings_to_apply[f"UserMenu{m_idx}_Count"] = (len(items), winreg.REG_DWORD)
                for i_idx, item_val in enumerate(items):
                    settings_to_apply[f"UserMenu{m_idx}_{i_idx}"] = (item_val, winreg.REG_DWORD)

        for k, v in extra_settings.items():
            settings_to_apply[k] = (v, winreg.REG_DWORD if isinstance(v, int) else winreg.REG_SZ)

        applied_per_root[root] = settings_to_apply

    try:
        for root, to_apply in applied_per_root.items():
            if to_apply:
                _write_reg_values(root, to_apply)

        yield
    finally:
        # Strict restore: Stirling rewrites its whole configuration on exit, so the key can
        # hold values this context manager never applied (e.g. StatusItem0.. after the env
        # settings dialog was used). Deleting everything that was not in the backup keeps
        # those out of later runs - they used to leak and make unrelated tests fail.
        for root, orig_values in backups.items():
            leftover = [n for n in _read_reg_values(root) if n not in orig_values]
            if leftover:
                _delete_reg_values(root, leftover)
            if orig_values:
                _write_reg_values(root, orig_values)


# Caret auto-restore store of StirHex (Issue #22).
#   HKCU\Software\StirHex\StirHex\CaretPositions
#     Count  REG_DWORD  number of entries (max 16)
#     Path%d REG_SZ     file path (UTF-16 since the Unicode build, Issue #41)
#     Addr%d REG_SZ     caret address, 64-bit, uppercase hex without prefix (current format)
#     Pos%d  REG_DWORD  caret address, 32-bit (legacy format written by the 32-bit build)
CARET_STORE_ROOT = r"Software\StirHex\StirHex\CaretPositions"


def read_caret_store() -> Dict[str, Any]:
    """Read the caret store as {value_name: (value, winreg type)}."""
    return _read_reg_values(CARET_STORE_ROOT)


@contextmanager
def caret_store(entries: Dict[str, Any] | None = None):
    """Replace the caret auto-restore store with `entries` for the duration of the test
    and strictly restore the previous contents afterwards.

    `entries` maps value name to (value, winreg type), e.g.
        {"Count": (1, winreg.REG_DWORD), "Path0": (str(path), winreg.REG_SZ),
         "Pos0": (0x40, winreg.REG_DWORD)}
    """
    backup = _read_reg_values(CARET_STORE_ROOT)
    _delete_reg_values(CARET_STORE_ROOT, list(backup.keys()))
    if entries:
        _write_reg_values(CARET_STORE_ROOT, entries)
    try:
        yield
    finally:
        current = _read_reg_values(CARET_STORE_ROOT)
        _delete_reg_values(CARET_STORE_ROOT, list(current.keys()))
        if backup:
            _write_reg_values(CARET_STORE_ROOT, backup)


# --- Seeding values the way the MBCS (ANSI) build wrote them (Issue #43) ---
#
# The registry stores strings as UTF-16 and the ANSI API converts REG_SZ with the system
# ANSI code page. The MBCS build of the ported Stirling handed CP932 bytes to
# CWinApp::WriteProfileString (i.e. RegSetValueExA), so reproducing a pre-Unicode
# installation means writing raw CP932 bytes through the *A* entry point - winreg always
# uses the wide API and would store a different byte sequence on a non-932 ACP.

_advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

_advapi32.RegCreateKeyExA.argtypes = [
    wintypes.HKEY, wintypes.LPCSTR, wintypes.DWORD, wintypes.LPSTR, wintypes.DWORD,
    wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(wintypes.HKEY), ctypes.POINTER(wintypes.DWORD),
]
_advapi32.RegCreateKeyExA.restype = wintypes.LONG
_advapi32.RegSetValueExA.argtypes = [
    wintypes.HKEY, wintypes.LPCSTR, wintypes.DWORD, wintypes.DWORD,
    ctypes.c_char_p, wintypes.DWORD,
]
_advapi32.RegSetValueExA.restype = wintypes.LONG
_advapi32.RegCloseKey.argtypes = [wintypes.HKEY]
_advapi32.RegCloseKey.restype = wintypes.LONG


def write_ansi_string_values(root_key_path: str, values: Dict[str, str]) -> None:
    """Write REG_SZ values under HKCU\\<root_key_path> through the ANSI registry API,
    passing CP932 bytes exactly as the MBCS build of Stirling did.

    On a Japanese system (ACP=932) this is indistinguishable from a wide write; on any
    other ACP the stored UTF-16 differs, which is what the migration has to repair.
    """
    hkey = wintypes.HKEY()
    rc = _advapi32.RegCreateKeyExA(
        wintypes.HKEY(winreg.HKEY_CURRENT_USER), root_key_path.encode("cp932"),
        0, None, 0, winreg.KEY_SET_VALUE, None, ctypes.byref(hkey), None,
    )
    if rc != 0:
        raise OSError(f"RegCreateKeyExA failed for {root_key_path!r}: {rc}")
    try:
        for name, text in values.items():
            data = text.encode("cp932") + b"\x00"
            rc = _advapi32.RegSetValueExA(
                hkey, name.encode("ascii"), 0, winreg.REG_SZ, data, len(data),
            )
            if rc != 0:
                raise OSError(f"RegSetValueExA failed for {root_key_path}\\{name}: {rc}")
    finally:
        _advapi32.RegCloseKey(hkey)


def read_reg_values(root_key_path: str) -> Dict[str, Any]:
    """Read all values under HKCU\\<root_key_path> as {name: (value, winreg type)}."""
    return _read_reg_values(root_key_path)


@contextmanager
def registry_section(root_key_path: str, entries: Dict[str, Any] | None = None):
    """Replace HKCU\\<root_key_path> with `entries` for the duration of the test and
    strictly restore the previous contents afterwards.

    `entries` maps value name to (value, winreg type). Values are written through the wide
    API; use write_ansi_string_values() inside the context to seed ANSI-written strings.
    """
    backup = _read_reg_values(root_key_path)
    _delete_reg_values(root_key_path, list(backup.keys()))
    if entries:
        _write_reg_values(root_key_path, entries)
    try:
        yield
    finally:
        current = _read_reg_values(root_key_path)
        _delete_reg_values(root_key_path, list(current.keys()))
        if backup:
            _write_reg_values(root_key_path, backup)
