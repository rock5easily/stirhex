import struct
import time
from contextlib import contextmanager
from pathlib import Path

import pytest
import win32con
import win32gui

from drivers.settings_context import registry_section
from drivers.stirling_driver import (
    ID_GOTO_DATA_END,
    StirlingDriver,
    _control_text,
)


PORT_ENV = r"Software\StirHex\StirHex\Env"
RAW_MARK2 = 0x070B
RAW_MARK3 = 0x070C
VK_F10 = 0x79
VK_F11 = 0x7A


def _mark_entries(drv: StirlingDriver) -> list[tuple[str, int]]:
    dialog = drv.open_mark_list_dialog()
    try:
        return drv.mark_list_entries(dialog)
    finally:
        drv.click_dialog_button(dialog, win32con.IDCANCEL)


def _find_optional_dialog(
    drv: StirlingDriver, title: str, timeout: float = 1.0
) -> int | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        for hwnd, cls, caption in drv._get_process_windows():
            if cls == "#32770" and caption == title:
                return hwnd
        time.sleep(0.05)
    return None


def _dismiss_dialog(dialog: int, button_id: int = win32con.IDOK):
    buttons = []

    def _find_button(hwnd, _):
        if win32gui.GetClassName(hwnd) == "Button":
            buttons.append(
                (hwnd, win32gui.GetDlgCtrlID(hwnd), _control_text(hwnd))
            )
        return True

    win32gui.EnumChildWindows(dialog, _find_button, None)
    matching = [item for item in buttons if item[1] == button_id]
    if not matching and len(buttons) == 1 and buttons[0][2] == "OK":
        matching = buttons
    assert matching, f"Dialog button {button_id} not found: {buttons}"
    button = matching[0][0]
    assert win32gui.GetAncestor(button, 2) == dialog  # GA_ROOT
    win32gui.SendMessage(button, win32con.BM_CLICK, 0, 0)
    deadline = time.time() + 5.0
    while time.time() < deadline and win32gui.IsWindow(dialog):
        time.sleep(0.05)
    assert not win32gui.IsWindow(dialog)


@contextmanager
def _temporary_struct_def(ported_exe_path: Path, tmp_path: Path):
    worktree_root = Path(__file__).resolve().parents[5]
    build_root = (worktree_root / "porting" / "StirHex").resolve()
    executable = Path(ported_exe_path).resolve()
    assert executable.is_relative_to(build_root), (
        f"Refusing to replace Struct.def outside Issue #78 build artifacts: {executable}"
    )
    target = (executable.parent / "Struct.def").resolve()
    assert target.parent == executable.parent
    assert target.is_relative_to(build_root)

    existed = target.exists()
    original = target.read_bytes() if existed else b""
    backup = tmp_path / "Struct.def.backup"
    backup.write_bytes(original)
    try:
        yield target
    finally:
        if existed:
            target.write_bytes(backup.read_bytes())
            assert target.read_bytes() == original
        elif target.exists():
            target.unlink()


STRUCT_INVALID_CASES = [
    pytest.param(
        "forward_reference",
        "struct Outer { Later value; };\nstruct Later { BYTE value; };\n",
        ["Outer"],
        True,
        id="forward-reference",
    ),
    pytest.param(
        "undefined_type",
        "struct Bad { Missing value; };\n",
        ["Bad"],
        True,
        id="undefined-type",
    ),
    pytest.param(
        "zero_array",
        "struct Bad { BYTE values[0]; };\n",
        ["Bad"],
        True,
        id="zero-array",
    ),
    pytest.param(
        "oversize_array",
        "struct Bad { BYTE values[65537]; };\n",
        ["Bad"],
        True,
        id="oversize-array",
    ),
    pytest.param(
        "non_ascii_identifier",
        "struct 日本語 { BYTE value; };\n",
        [],
        True,
        id="non-ascii-identifier",
    ),
    pytest.param(
        "pointer",
        "struct Pointer { BYTE *value; };\n",
        ["Pointer"],
        False,
        id="pointer",
    ),
    pytest.param(
        "bit_field",
        "struct BitField { BYTE value:1; };\n",
        ["BitField"],
        False,
        id="bit-field",
    ),
    pytest.param(
        "typedef",
        "typedef struct Alias { BYTE value; } Alias;\n",
        ["Alias"],
        False,
        id="typedef",
    ),
    pytest.param(
        "unsigned",
        "struct Unsigned { unsigned long value; };\n",
        ["Unsigned"],
        True,
        id="unsigned",
    ),
    pytest.param(
        "function",
        "struct Function { BYTE value(); };\n",
        ["Function"],
        False,
        id="function",
    ),
    pytest.param(
        "multiple_declaration",
        "struct Multiple { BYTE first, second; };\n",
        ["Multiple"],
        False,
        id="multiple-declaration",
    ),
    pytest.param(
        "inline_nested",
        "struct Outer { struct { BYTE value; } nested; };\n",
        ["Outer"],
        True,
        id="inline-nested",
    ),
    pytest.param("empty", "", [], False, id="empty"),
    pytest.param(
        "valid_then_invalid",
        "struct Good { BYTE value; };\nstruct Bad { Missing value; };\n",
        ["Good", "Bad"],
        True,
        id="valid-then-invalid",
    ),
]


class TestHelpKeymapStructBehavior:
    @pytest.mark.ported
    def test_hv037_mark2_mark3_assignment_and_toggle(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "mark_types.dat"
        test_file.write_bytes(bytes(range(64)))

        with registry_section(PORT_ENV):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                sheet, page = drv.open_key_assign_page()
                drv.key_assign_set_modifiers(page)
                keys = drv.key_assign_keys(page)
                assert keys[9] == "F10" and keys[10] == "F11"
                drv.key_assign_select_key(page, 9)
                drv.key_assign_select_function(page, RAW_MARK2)
                drv.key_assign_select_key(page, 10)
                drv.key_assign_select_function(page, RAW_MARK3)
                drv.close_settings_sheet(sheet, accept=True)

                drv.jump_to_address("10", is_hex=True)
                drv.send_vk(VK_F10)
                assert _mark_entries(drv) == [("00000010", 1)]
                drv.send_vk(VK_F10)
                assert _mark_entries(drv) == []

                drv.send_vk(VK_F10)
                drv.send_vk(VK_F11)
                assert _mark_entries(drv) == [("00000010", 2)]
                drv.mark_toggle()
                assert _mark_entries(drv) == [("00000010", 0)]
                drv.send_vk(VK_F10)
                assert _mark_entries(drv) == [("00000010", 1)]

                drv.post_command(ID_GOTO_DATA_END)
                drv.send_vk(VK_F11)
                assert _mark_entries(drv) == [("00000010", 1)]

    @pytest.mark.ported
    @pytest.mark.parametrize(
        "case_name,definition,expected_names,expect_message",
        STRUCT_INVALID_CASES,
    )
    def test_hv042_struct_def_invalid_and_unsupported_syntax(
        self,
        ported_exe_path,
        tmp_path,
        case_name,
        definition,
        expected_names,
        expect_message,
    ):
        test_file = tmp_path / f"struct_{case_name}.dat"
        test_file.write_bytes(bytes(range(64)))

        with _temporary_struct_def(ported_exe_path, tmp_path) as struct_def:
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.toggle_struct_bar(show=True)
                struct_def.write_bytes(definition.encode("cp932"))
                drv.reload_struct_def()

                message = _find_optional_dialog(drv, "StirHex")
                if message is not None:
                    child_texts = []

                    def _collect(hwnd, _):
                        text = _control_text(hwnd)
                        if text:
                            child_texts.append(text)
                        return True

                    win32gui.EnumChildWindows(message, _collect, None)
                    assert any(text != "OK" for text in child_texts)
                    _dismiss_dialog(message)
                assert (message is not None) == expect_message
                assert drv.pid > 0 and win32gui.IsWindow(drv.hwnd)
                assert drv.struct_type_names() == expected_names

    @pytest.mark.ported
    def test_hv048_key_assign_catalog_assignment_cancel_and_reset(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "key_assign.dat"
        test_file.write_bytes(bytes(range(64)))

        with registry_section(PORT_ENV):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                sheet, page = drv.open_key_assign_page()
                assert drv.key_assign_categories(page) == [
                    "ファイル系",
                    "カーソル移動系",
                    "選択系",
                    "編集系",
                    "検索・置換系",
                    "メニュー系",
                    "ウィンドウ系",
                    "その他",
                ]

                expected_key_counts = {
                    (False, False): 15,
                    (False, True): 23,
                    (True, False): 57,
                    (True, True): 57,
                }
                for modifiers, expected_count in expected_key_counts.items():
                    ctrl, shift = modifiers
                    drv.key_assign_set_modifiers(page, ctrl=ctrl, shift=shift)
                    keys = drv.key_assign_keys(page)
                    assert len(keys) == expected_count
                    prefix = (
                        "Ctrl + Shift + " if ctrl and shift else
                        "Ctrl + " if ctrl else
                        "Shift + " if shift else ""
                    )
                    assert all(key.startswith(prefix) for key in keys)

                raw_ids = []
                for category in range(8):
                    drv.key_assign_select_category(page, category)
                    raw_ids.extend(
                        raw for _name, raw in drv.key_assign_functions(page) if raw != 0
                    )
                assert len(raw_ids) == 112
                assert len(set(raw_ids)) == 112
                assert RAW_MARK2 in raw_ids and RAW_MARK3 in raw_ids

                drv.key_assign_set_modifiers(page)
                drv.key_assign_select_key(page, 9)
                drv.key_assign_select_function(page, RAW_MARK2)
                drv.close_settings_sheet(sheet, accept=False)

                sheet, page = drv.open_key_assign_page()
                drv.key_assign_set_modifiers(page)
                drv.key_assign_select_key(page, 9)
                assert drv.key_assign_current_function(page) == 0
                drv.key_assign_select_function(page, RAW_MARK2)
                drv.key_assign_reset(page)
                confirm = drv.find_process_dialog("StirHex")
                _dismiss_dialog(confirm, win32con.IDCANCEL)
                assert drv.key_assign_current_function(page) == RAW_MARK2

                drv.key_assign_reset(page)
                confirm = drv.find_process_dialog("StirHex")
                _dismiss_dialog(confirm, win32con.IDOK)
                drv.key_assign_select_key(page, 9)
                assert drv.key_assign_current_function(page) == 0

                drv.key_assign_select_function(page, RAW_MARK2)
                drv.close_settings_sheet(sheet, accept=True)
                drv.send_vk(VK_F10)
                assert _mark_entries(drv) == [("00000000", 1)]

    @pytest.mark.ported
    def test_hv048_key_file_export_import_and_invalid_length(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "key_file.dat"
        test_file.write_bytes(bytes(range(64)))
        exported = (tmp_path / "exported.key").resolve()
        imported = (tmp_path / "imported.key").resolve()
        invalid = (tmp_path / "invalid.key").resolve()
        assert all(
            path.is_relative_to(tmp_path.resolve())
            for path in (exported, imported, invalid)
        )

        imported_values = [0] * 256
        imported_values[10] = RAW_MARK3
        imported.write_bytes(struct.pack("<256H", *imported_values))
        invalid.write_bytes(b"\x00" * 511)

        with registry_section(PORT_ENV):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                sheet, page = drv.open_key_assign_page()
                drv.key_assign_set_modifiers(page)
                drv.key_assign_select_key(page, 9)
                drv.key_assign_select_function(page, RAW_MARK2)
                drv.key_assign_transfer_file(page, exported, save=True)

                exported_data = exported.read_bytes()
                assert len(exported_data) == 512
                values = struct.unpack("<256H", exported_data)
                assert len(values) == 256 and values[9] == RAW_MARK2

                drv.key_assign_transfer_file(page, imported, save=False)
                drv.key_assign_select_key(page, 10)
                assert drv.key_assign_current_function(page) == RAW_MARK3

                drv.key_assign_transfer_file(page, invalid, save=False)
                error = drv.find_process_dialog("StirHex")
                assert win32gui.IsWindow(error)
                _dismiss_dialog(error)
                drv.key_assign_select_key(page, 10)
                assert drv.key_assign_current_function(page) == RAW_MARK3

                drv.close_settings_sheet(sheet, accept=True)
                drv.send_vk(VK_F11)
                assert _mark_entries(drv) == [("00000000", 2)]
