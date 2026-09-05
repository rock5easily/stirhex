import ctypes
import os
import time
import winreg
from ctypes import wintypes
from pathlib import Path
from contextlib import contextmanager
from typing import Any, Dict


# Settings roots, written in registry form. The original Stirling really does live in
# the registry; the StirHex roots are redirected to sections of its settings file by the
# helpers below (Issue #96).
# - Original Stirling: HKCU\Software\DDS2\Stirling\Settings
# - StirHex: the [Env] section of the settings file
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


# --- StirHex settings file (Issue #96) ---
#
# StirHex no longer keeps its settings in the registry: CWinApp's profile API is backed by
# an INI-style file (UTF-8, no BOM). The registry root below is only read once, on the very
# first start, to carry a pre-1.1.0 installation over.
#
# The helpers keep their registry-shaped signatures so the tests do not have to change:
# a root under STIRHEX_REG_ROOT is redirected to the matching section of the settings file,
# and anything else (the original Stirling, the legacy StirlingPort root) still goes to the
# registry.
STIRHEX_REG_ROOT = r"Software\StirHex\StirHex"

# The app resolves its settings file as: /ini:<path>, then StirHex.ini next to the exe if
# that file exists, then %APPDATA%\StirHex\StirHex.ini. The tests drive the exe from the
# build tree, where no portable ini is committed, so the APPDATA path is what they see.
# STIRHEX_SETTINGS_FILE overrides it for a run against a portable install.
def settings_file_path() -> Path:
    override = os.environ.get("STIRHEX_SETTINGS_FILE")
    if override:
        return Path(override)
    return Path(os.environ["APPDATA"]) / "StirHex" / "StirHex.ini"


def _section_of(root_key_path: str) -> str | None:
    """Return the settings-file section a registry root maps to, or None if it is a real
    registry path (the original Stirling / the legacy port root)."""
    prefix = STIRHEX_REG_ROOT + "\\"
    if root_key_path == STIRHEX_REG_ROOT:
        return ""
    if root_key_path.startswith(prefix):
        return root_key_path[len(prefix):]
    return None


def _decode_ini_value(text: str) -> str:
    """Undo the quoting the app applies to values that cannot be written raw."""
    if len(text) < 2 or not text.startswith('"') or not text.endswith('"'):
        return text
    out = []
    i = 1
    end = len(text) - 1
    while i < end:
        c = text[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        if i + 1 >= end:
            out.append("\\")
            break
        esc = text[i + 1]
        i += 2
        simple = {"\\": "\\", '"': '"', "r": "\r", "n": "\n", "t": "\t"}
        if esc in simple:
            out.append(simple[esc])
        elif esc == "x":
            digits = ""
            while len(digits) < 4 and i < end and text[i] in "0123456789abcdefABCDEF":
                digits += text[i]
                i += 1
            out.append(chr(int(digits, 16)) if digits else "\\x")
        else:
            out.append("\\")
            out.append(esc)
    return "".join(out)


def _encode_ini_value(text: str) -> str:
    """Quote a value the same way the app does (only when it cannot be written raw)."""
    needs_quote = bool(text) and (
        text[0] in " \t" or text[-1] in " \t" or text[0] == '"'
    )
    if not needs_quote:
        needs_quote = any(ord(c) < 0x20 or ord(c) == 0x7F for c in text)
    if not needs_quote:
        return text
    out = ['"']
    for c in text:
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\r":
            out.append("\\r")
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif ord(c) < 0x20 or ord(c) == 0x7F:
            out.append("\\x%04X" % ord(c))
        else:
            out.append(c)
    out.append('"')
    return "".join(out)


def _read_settings_text() -> str | None:
    """Read the settings file, retrying while the app is replacing it.

    The app writes a temp file and swaps it in, so a read that lands on the swap fails
    with a sharing violation. The swap is over in microseconds - retry briefly rather
    than let a test flake on it. Returns None when the file does not exist.
    """
    path = settings_file_path()
    for attempt in range(20):
        if not path.exists():
            return None
        try:
            return path.read_text(encoding="utf-8")
        except (PermissionError, OSError):
            if attempt == 19:
                raise
            time.sleep(0.05)
    return None


def _read_ini() -> Dict[str, Dict[str, str]]:
    """Read the settings file as {section: {key: value}} (empty if it does not exist)."""
    text = _read_settings_text()
    if text is None:
        return {}
    sections: Dict[str, Dict[str, str]] = {}
    current = ""
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line[0] in ";#":
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip()
            sections.setdefault(current, {})
            continue
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        if not key:
            continue
        sections.setdefault(current, {})[key] = _decode_ini_value(value.strip())
    return sections


def _write_ini(sections: Dict[str, Dict[str, str]]) -> None:
    """Write the whole settings file back, creating the folder if needed.

    Retries like the reader: the app may be swapping the file in at this moment.
    """
    path = settings_file_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    chunks = []
    for name, entries in sections.items():
        chunks.append(f"[{name}]\r\n")
        for key, value in entries.items():
            chunks.append(f"{key}={_encode_ini_value(value)}\r\n")
        chunks.append("\r\n")
    payload = "".join(chunks)
    for attempt in range(20):
        try:
            path.write_text(payload, encoding="utf-8", newline="")
            return
        except (PermissionError, OSError):
            if attempt == 19:
                raise
            time.sleep(0.05)


def _typed(value: str) -> Any:
    """Present a settings-file value the way the registry helpers used to.

    Everything is text in the file; ints are reported as REG_DWORD so the assertions the
    tests already make on (value, type) pairs keep working.
    """
    try:
        return (int(value), winreg.REG_DWORD)
    except ValueError:
        return (value, winreg.REG_SZ)


def _read_ini_values(section: str) -> Dict[str, Any]:
    return {name: _typed(text) for name, text in _read_ini().get(section, {}).items()}


def _write_ini_values(section: str, values: Dict[str, Any]) -> None:
    sections = _read_ini()
    entries = sections.setdefault(section, {})
    for name, (value, _val_type) in values.items():
        entries[name] = str(value)
    _write_ini(sections)


def _delete_ini_values(section: str, names: list[str]) -> None:
    sections = _read_ini()
    # setdefault (not get) so the file exists even when a test only wipes a section:
    # a missing settings file would make the app re-import the real registry settings.
    entries = sections.setdefault(section, {})
    for name in names:
        entries.pop(name, None)
    _write_ini(sections)


def _read_reg_values(root_key_path: str) -> Dict[str, Any]:
    """Read all values of a settings root as {name: (value, winreg type)}.

    StirHex roots come from the settings file, everything else from the registry.
    """
    section = _section_of(root_key_path)
    if section is not None:
        return _read_ini_values(section)
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
    """Write values to a settings root, creating the key or file if needed."""
    section = _section_of(root_key_path)
    if section is not None:
        _write_ini_values(section, values)
        return
    try:
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, root_key_path) as key:
            for name, (val, val_type) in values.items():
                winreg.SetValueEx(key, name, 0, val_type, val)
    except Exception:
        pass


def _delete_reg_values(root_key_path: str, names: list[str]):
    """Delete the named values from a settings root."""
    section = _section_of(root_key_path)
    if section is not None:
        _delete_ini_values(section, names)
        return
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
    and StirHex (the [Env] section of its settings file).
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


@contextmanager
def settings_value(root_key_path: str, name: str, value: Any,
                   val_type: int = winreg.REG_DWORD):
    """Temporarily replace a single value in a settings root, restoring it afterwards.

    Narrower than registry_section(), which wipes the whole section: use this when the
    other values in the section have to stay as they are.
    """
    previous = _read_reg_values(root_key_path).get(name)
    _write_reg_values(root_key_path, {name: (value, val_type)})
    try:
        yield
    finally:
        if previous is None:
            _delete_reg_values(root_key_path, [name])
        else:
            _write_reg_values(root_key_path, {name: previous})


@contextmanager
def deleted_settings_values(root_key_path: str, names: list[str]):
    """Temporarily remove values from a settings root, restoring them afterwards.

    The counterpart of settings_value() for "this key must not be present": use it to
    exercise the code path an absent key takes, without wiping the whole section the way
    registry_section() does.
    """
    previous = {n: v for n, v in _read_reg_values(root_key_path).items() if n in names}
    _delete_reg_values(root_key_path, list(previous.keys()))
    try:
        yield
    finally:
        current = _read_reg_values(root_key_path)
        leftover = [n for n in names if n in current]
        if leftover:
            _delete_reg_values(root_key_path, leftover)
        if previous:
            _write_reg_values(root_key_path, previous)


# Caret auto-restore store of StirHex (Issue #22).
#   the [CaretPositions] section of the settings file
#     Count  number of entries (max 16)
#     Path%d file path (UTF-8 in the file; UTF-16 in memory since the Unicode build, Issue #41)
#     Addr%d caret address, 64-bit, uppercase hex without prefix (current format)
#     Pos%d  caret address, 32-bit (legacy format written by the 32-bit build)
CARET_STORE_ROOT = r"Software\StirHex\StirHex\CaretPositions"


def read_caret_store() -> Dict[str, str]:
    """Read the caret store as {value_name: text}.

    Raw text, not the (value, type) pairs the other readers return: the settings file has
    no value types, and what the caret assertions care about is the text the app wrote -
    Addr%d is an uppercase hex address (Issue #22), which a numeric reading would hide.
    """
    section = _section_of(CARET_STORE_ROOT)
    assert section is not None, "the caret store lives in the StirHex settings file"
    return dict(_read_ini().get(section, {}))


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
    """Replace one settings root with `entries` for the duration of the test and strictly
    restore the previous contents afterwards.

    `entries` maps value name to (value, winreg type). A StirHex root is redirected to the
    settings file, where everything is text and the type is only used to format the value.
    A real registry root is written through the wide API; use write_ansi_string_values()
    inside the context to seed ANSI-written strings.
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
