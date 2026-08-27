from contextlib import ExitStack
import time
import winreg

import pytest
import win32con
import win32gui

from drivers.settings_context import read_reg_values, registry_section
from drivers.stirling_driver import (
    CMD_FILE_SAVE,
    IDC_DISP_ADDR_HSCROLL,
    IDC_DISP_BO_LITTLE,
    IDC_DISP_CS_SJIS,
    IDC_DISP_LINESIZE,
    IDC_DISP_OPEN_CHARMODE,
    IDC_DISP_OPEN_INSERT,
    IDC_DISP_OPEN_READONLY,
    IDC_DISP_RADIX_HEX,
    IDC_EXTLIST_DELETE,
    IDC_EXTREC_COMMENT,
    IDC_EXTREC_EXT,
    IDC_TBAR_ADD,
    IDC_TBAR_DELETE,
    IDC_TBAR_DOWN,
    IDC_TBAR_SEPARATOR,
    IDC_TBAR_UP,
    StirlingDriver,
    _control_text,
)


ROOT = r"Software\StirHex\StirHex"
ENV = rf"{ROOT}\Env"
EXTENSIONS = rf"{ROOT}\Extensions"

DEFAULT_TOOLBAR = [
    0x0001, 0x0002, 0x0004, 0xFFFF, 0x0300, 0x0301, 0xFFFF,
    0x0302, 0x0303, 0x0304, 0x0305, 0x030E, 0xFFFF, 0x0400,
    0x0404, 0x0408, 0x0409, 0x040A, 0x040B, 0x040C, 0xFFFF,
    0x0104, 0x0105, 0x0110, 0xFFFF, 0x0700, 0x0701, 0x0702,
    0xFFFF, 0x000A, 0x0709,
]


def _extension_context(stack: ExitStack, records: int = 0):
    stack.enter_context(registry_section(
        EXTENSIONS, {"Count": (records, winreg.REG_DWORD)}
    ))
    for index in range(max(records, 4)):
        stack.enter_context(registry_section(rf"{ROOT}\Rec{index}"))


def _seed_extension_records(stack: ExitStack, patterns, settings):
    entries = {"Count": (len(patterns), winreg.REG_DWORD)}
    for index, (extension, comment) in enumerate(patterns):
        entries[f"Ext{index}"] = (extension, winreg.REG_SZ)
        entries[f"Comment{index}"] = (comment, winreg.REG_SZ)
    stack.enter_context(registry_section(EXTENSIONS, entries))
    for index, values in enumerate(settings):
        stack.enter_context(registry_section(
            rf"{ROOT}\Rec{index}", {
                name: (value, winreg.REG_DWORD)
                for name, value in values.items()
            }
        ))
    for index in range(len(settings), 6):
        stack.enter_context(registry_section(rf"{ROOT}\Rec{index}"))


def _has_dlg_item(dialog: int, control_id: int) -> bool:
    try:
        return bool(win32gui.GetDlgItem(dialog, control_id))
    except Exception:
        return False


class TestHelpSettingsBehavior:
    @pytest.mark.ported
    def test_hv050_toolbar_catalog_edit_cancel_ok_and_rebuild(self, ported_exe_path):
        with registry_section(ENV):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                initial_button_count = drv.toolbar_button_count()

                sheet, page = drv.open_toolbar_page()
                assert drv.toolbar_items(page, current=True) == DEFAULT_TOOLBAR
                categories = drv.toolbar_categories(page)
                assert len(categories) == 8
                represented_categories = set()
                for category in range(8):
                    drv.toolbar_select_category(page, category)
                    available = drv.toolbar_items(page, current=False)
                    if available or any(
                        raw != 0xFFFF and (raw >> 8) == category
                        for raw in DEFAULT_TOOLBAR
                    ):
                        represented_categories.add(category)
                assert represented_categories == {0, 1, 2, 3, 4, 6, 7}
                drv.toolbar_select_category(page, 5)
                assert drv.toolbar_items(page, current=False) == []
                assert not drv.toolbar_button_states(page)[IDC_TBAR_ADD]

                drv.toolbar_select_category(page, 0)
                available = drv.toolbar_items(page, current=False)
                added_raw = available[0]
                drv.toolbar_select_item(page, 0, current=False)
                drv.toolbar_click(page, IDC_TBAR_ADD)
                assert added_raw not in drv.toolbar_items(page, current=False)
                drv.toolbar_click(page, IDC_TBAR_DELETE)
                assert drv.toolbar_items(page, current=True) == DEFAULT_TOOLBAR
                assert added_raw in drv.toolbar_items(page, current=False)
                added_index = drv.toolbar_items(page, current=False).index(added_raw)
                drv.toolbar_select_item(page, added_index, current=False)
                drv.toolbar_click(page, IDC_TBAR_ADD)
                drv.close_settings_sheet(sheet, accept=False)

                sheet, page = drv.open_toolbar_page()
                assert drv.toolbar_items(page, current=True) == DEFAULT_TOOLBAR
                drv.toolbar_select_category(page, 0)
                drv.toolbar_select_item(page, 0, current=False)
                drv.toolbar_click(page, IDC_TBAR_ADD)
                after_add = drv.toolbar_items(page, current=True)
                inserted = after_add.index(added_raw)
                assert inserted == 1

                drv.toolbar_click(page, IDC_TBAR_SEPARATOR)
                after_separator = drv.toolbar_items(page, current=True)
                assert after_separator[inserted + 1] == 0xFFFF
                states = drv.toolbar_button_states(page)
                assert states[IDC_TBAR_DELETE] and states[IDC_TBAR_UP]
                assert states[IDC_TBAR_DOWN]

                drv.toolbar_click(page, IDC_TBAR_DOWN)
                assert drv.toolbar_items(page, current=True)[inserted + 2] == 0xFFFF
                drv.toolbar_click(page, IDC_TBAR_UP)
                assert drv.toolbar_items(page, current=True)[inserted + 1] == 0xFFFF
                drv.close_settings_sheet(sheet, accept=True)

                assert drv.toolbar_button_count() == initial_button_count + 2
                saved = read_reg_values(ENV)
                assert saved["ToolbarItemCount"][0] == len(DEFAULT_TOOLBAR) + 2

    @pytest.mark.ported
    def test_hv052_extension_list_add_edit_cancel_delete(self, ported_exe_path):
        with ExitStack() as stack:
            _extension_context(stack)
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                dialog = drv.open_extension_settings_dialog()
                assert drv.extension_list_rows(dialog) == ["(*.*)すべてのファイル"]
                assert not _has_dlg_item(dialog, 1073)
                assert not _has_dlg_item(dialog, 1074)

                drv.extension_select(dialog, 0)
                drv.click_dialog_button(dialog, IDC_EXTLIST_DELETE)
                message, _title, items = drv.find_message_box()
                assert items
                win32gui.PostMessage(message, win32con.WM_COMMAND, win32con.IDOK, 0)
                time.sleep(0.2)
                assert drv.extension_list_rows(dialog) == ["(*.*)すべてのファイル"]

                record, display = drv.extension_open_record(dialog)
                ext_edit = win32gui.GetDlgItem(record, IDC_EXTREC_EXT)
                comment_edit = win32gui.GetDlgItem(record, IDC_EXTREC_COMMENT)
                assert _control_text(ext_edit) == "*"
                assert not win32gui.IsWindowEnabled(ext_edit)
                assert not win32gui.IsWindowEnabled(comment_edit)
                assert _control_text(win32gui.GetDlgItem(display, IDC_DISP_LINESIZE)) == "16"
                assert win32gui.SendMessage(
                    win32gui.GetDlgItem(display, IDC_DISP_RADIX_HEX),
                    win32con.BM_GETCHECK, 0, 0,
                ) == win32con.BST_CHECKED
                assert win32gui.SendMessage(
                    win32gui.GetDlgItem(display, IDC_DISP_CS_SJIS),
                    win32con.BM_GETCHECK, 0, 0,
                ) == win32con.BST_CHECKED
                assert win32gui.SendMessage(
                    win32gui.GetDlgItem(display, IDC_DISP_BO_LITTLE),
                    win32con.BM_GETCHECK, 0, 0,
                ) == win32con.BST_CHECKED
                for control_id in (
                    IDC_DISP_ADDR_HSCROLL, IDC_DISP_OPEN_READONLY,
                    IDC_DISP_OPEN_INSERT, IDC_DISP_OPEN_CHARMODE,
                ):
                    assert win32gui.SendMessage(
                        win32gui.GetDlgItem(display, control_id),
                        win32con.BM_GETCHECK, 0, 0,
                    ) == win32con.BST_UNCHECKED
                drv.close_dialog(record, accept=False)

                record, display = drv.extension_open_record(dialog, add=True)
                drv.extension_set_header(record, "txt; log ;CFG", "取消対象")
                drv.close_dialog(record, accept=False)
                assert len(drv.extension_list_rows(dialog)) == 1

                record, display = drv.extension_open_record(dialog, add=True)
                drv.extension_set_header(record, "txt; log ;CFG", "テスト設定")
                drv.extension_configure_display(display, line_size=32)
                drv.close_dialog(record, accept=True)
                assert drv.extension_list_rows(dialog)[-1] == (
                    "(*.TXT; LOG ;CFG)テスト設定"
                )

                drv.extension_select(dialog, 1)
                record, _display = drv.extension_open_record(
                    dialog, double_click=True
                )
                drv.extension_set_header(record, "txt; log ;CFG", "編集済み")
                drv.close_dialog(record, accept=True)
                assert drv.extension_list_rows(dialog)[1].endswith("編集済み")

                drv.extension_select(dialog, 1)
                delete_button = win32gui.GetDlgItem(dialog, IDC_EXTLIST_DELETE)
                win32gui.SendMessage(
                    dialog, win32con.WM_COMMAND, IDC_EXTLIST_DELETE, delete_button
                )
                time.sleep(0.2)
                assert len(drv.extension_list_rows(dialog)) == 1

                record, display = drv.extension_open_record(dialog, add=True)
                drv.extension_set_header(record, "txt; log ;CFG", "保存対象")
                drv.extension_configure_display(display, line_size=32)
                drv.close_dialog(record, accept=True)
                assert drv.extension_list_rows(dialog)[1].endswith("保存対象")
                win32gui.PostMessage(dialog, win32con.WM_CLOSE, 0, 0)
                time.sleep(0.2)

    @pytest.mark.ported
    def test_hv052_hv053_extension_matching_layout_and_open_modes(
        self, ported_exe_path, tmp_path
    ):
        patterns = [
            ("*", "すべてのファイル"),
            ("TXT; log ;CFG", "先勝ち"),
            ("TXT", "後勝ちしない"),
        ]
        settings = [
            {"LineSize": 16, "AddressBase": 1},
            {
                "LineSize": 8, "AddressBase": 0, "AddrHScroll": 1,
                "OpenReadOnly": 1, "OpenInsert": 1, "OpenCharMode": 1,
                "CharSet": 0, "ByteOrder": 1,
            },
            {"LineSize": 32, "AddressBase": 1},
        ]
        files = {
            "default": tmp_path / "fallback.bin",
            "upper": tmp_path / "upper.TXT",
            "lower": tmp_path / "lower.log",
            "mixed": tmp_path / "mixed.CfG",
            "partial": tmp_path / "partial.txtx",
            "none": tmp_path / "no_extension",
            "multi": tmp_path / "archive.part.txt",
        }
        for path in files.values():
            path.write_bytes(bytes(range(256)) * 4)

        env_entries = {
            "DocMaximize": (0, winreg.REG_DWORD),
            "StatusItemCount": (4, winreg.REG_DWORD),
            "StatusItem0": (0xE704, winreg.REG_DWORD),
            "StatusItem1": (0xE707, winreg.REG_DWORD),
            "StatusItem2": (0xE70D, winreg.REG_DWORD),
            "StatusItem3": (0xE716, winreg.REG_DWORD),
        }
        with ExitStack() as stack:
            stack.enter_context(registry_section(ENV, env_entries))
            _seed_extension_records(stack, patterns, settings)
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(files["default"])

                def active_width():
                    active = drv.active_mdi_title()
                    child = next(
                        child for title, child, _view in drv.get_mdi_views()
                        if title == active
                    )
                    rect = win32gui.GetWindowRect(child)
                    return rect[2] - rect[0]

                default_width = active_width()
                widths = {}
                for key in ("upper", "lower", "mixed", "partial", "none", "multi"):
                    drv.open_file_via_dialog(files[key])
                    widths[key] = active_width()
                    if key == "upper":
                        panes = drv.get_all_statusbar_text()
                        assert "ASCII" in panes
                        assert "BigEndian" in panes
                        assert any(text for text in panes[1:3])

                assert widths["upper"] == widths["lower"] == widths["mixed"]
                assert widths["upper"] == widths["multi"]
                assert widths["upper"] < default_width
                assert widths["partial"] == widths["none"] == default_width

    @pytest.mark.ported
    def test_hv053_open_char_mode_is_applied(self, ported_exe_path, tmp_path):
        char_file = tmp_path / "open_char.char"
        hex_file = tmp_path / "open_hex.hex"
        char_file.write_bytes(bytes(range(64)))
        hex_file.write_bytes(bytes(range(64)))
        patterns = [
            ("*", "すべてのファイル"),
            ("CHAR", "文字入力"),
            ("HEX", "16進入力"),
        ]
        settings = [
            {"LineSize": 16},
            {"LineSize": 24, "OpenCharMode": 1, "CharSet": 0},
            {"LineSize": 24, "OpenCharMode": 0, "CharSet": 0},
        ]
        with ExitStack() as stack:
            _seed_extension_records(stack, patterns, settings)
            with StirlingDriver(ported_exe_path) as drv:
                # OpenCharMode=1 starts in the text pane, so a text character edits
                # the first byte without an explicit pane toggle.
                drv.start(char_file)
                drv.type_text_chars("Z")
                drv.post_command(CMD_FILE_SAVE)
                time.sleep(0.4)
                assert char_file.read_bytes()[0] == ord("Z")

                # OpenCharMode=0 keeps the default hex pane. A non-hex character
                # must therefore leave the first byte unchanged.
                drv.open_file_via_dialog(hex_file)
                drv.type_text_chars("Z")
                drv.post_command(CMD_FILE_SAVE)
                time.sleep(0.4)
                assert hex_file.read_bytes()[0] == 0

                # Changing the extension record re-applies document settings, but
                # must not override the pane selected by the user for this view.
                drv.press_tab()
                dialog = drv.open_extension_settings_dialog()
                drv.extension_select(dialog, 2)
                record, display = drv.extension_open_record(dialog)
                drv.extension_configure_display(
                    display, line_size=32, char_mode=False
                )
                drv.close_dialog(record, accept=True)
                drv.close_dialog(dialog, accept=True)

                # The view remains in the text pane after the changed settings apply.
                drv.type_text_chars("Y")
                drv.post_command(CMD_FILE_SAVE)
                time.sleep(0.4)
                assert hex_file.read_bytes()[0] == ord("Y")

    @pytest.mark.ported
    def test_hv055_extension_accept_reapplies_and_persists(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "persist.bin"
        test_file.write_bytes(bytes(range(256)) * 4)
        with ExitStack() as stack:
            _extension_context(stack)
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                before_child = drv.get_mdi_views()[0][1]
                before_rect = win32gui.GetWindowRect(before_child)
                before_width = before_rect[2] - before_rect[0]

                dialog = drv.open_extension_settings_dialog()
                record, display = drv.extension_open_record(dialog, add=True)
                drv.extension_set_header(record, "BIN", "永続化")
                drv.extension_configure_display(
                    display, line_size=32, address_hex=False,
                    read_only=True, insert=True, char_mode=True,
                    charset=0, byte_order_big=True,
                )
                drv.close_dialog(record, accept=True)
                close_button = win32gui.GetDlgItem(dialog, win32con.IDOK)
                assert _control_text(close_button) == "閉じる(&C)"
                win32gui.SendMessage(close_button, win32con.BM_CLICK, 0, 0)

                deadline = time.time() + 3
                while (read_reg_values(EXTENSIONS).get("Count", (None,))[0] != 2
                       and time.time() < deadline):
                    time.sleep(0.1)
                saved = read_reg_values(EXTENSIONS)
                assert saved["Count"][0] == 2
                assert saved["Ext1"][0] == "BIN"

                after_child = drv.get_mdi_views()[0][1]
                after_rect = win32gui.GetWindowRect(after_child)
                assert after_rect[2] - after_rect[0] > before_width

            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                child = drv.get_mdi_views()[0][1]
                rect = win32gui.GetWindowRect(child)
                assert rect[2] - rect[0] > before_width
