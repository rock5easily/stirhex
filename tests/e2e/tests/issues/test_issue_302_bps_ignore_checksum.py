"""Issue #302: apply a BPS patch while ignoring source/target CRC mismatches."""

import pytest
import win32con
import win32gui

from drivers.stirling_driver import StirlingDriver
from tests.issues.test_issue_259_binary_patch import (
    APPLY_COMMAND,
    CREATE_COMMAND,
    FORMAT_NOTE,
    MODE_APPLY,
    MODE_CREATE,
    OPEN_RESULT,
    START,
    STATUS,
    click,
    close_patch_dialog,
    fill_apply,
    open_patch_dialog,
    post_click,
    post_message_box_button,
    text,
    wait_for,
    wait_message_box,
    write_bps_one_byte_change,
)

pytestmark = pytest.mark.ported

IGNORE_CHECKSUM = 1520


def is_checked(dialog, control):
    return win32gui.SendMessage(win32gui.GetDlgItem(dialog, control),
                                win32con.BM_GETCHECK, 0, 0) == win32con.BST_CHECKED


def visible(dialog, control):
    return bool(win32gui.IsWindowVisible(win32gui.GetDlgItem(dialog, control)))


def make_mismatched_inputs(tmp_path):
    """Return (source, patch, expected) where source differs from the patch's source."""
    original = bytes(range(64))
    patch = tmp_path / "change.bps"
    write_bps_one_byte_change(patch, original, 10, 0xEE)
    source = tmp_path / "other.bin"
    modified = bytearray(original)
    modified[40] ^= 0xFF
    source.write_bytes(bytes(modified))
    expected = bytearray(modified)
    expected[10] = 0xEE
    return source, patch, bytes(expected)


def test_ignore_checksum_option_is_apply_only_and_off_by_default(ported_exe_path, tmp_path):
    source, patch, _expected = make_mismatched_inputs(tmp_path)
    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, CREATE_COMMAND)
        assert not visible(dialog, IGNORE_CHECKSUM)
        click(dialog, MODE_APPLY)
        wait_for(lambda: visible(dialog, IGNORE_CHECKSUM), "option shown in apply mode")
        assert not is_checked(dialog, IGNORE_CHECKSUM)

        fill_apply(dialog, source, patch, tmp_path / "result.bin")
        wait_for(lambda: "CRCを検証" in text(dialog, FORMAT_NOTE), "BPS verified note")
        click(dialog, IGNORE_CHECKSUM)
        wait_for(lambda: "不一致を無視" in text(dialog, FORMAT_NOTE), "BPS ignore note")
        click(dialog, IGNORE_CHECKSUM)
        wait_for(lambda: "CRCを検証" in text(dialog, FORMAT_NOTE), "BPS verified note again")

        click(dialog, MODE_CREATE)
        wait_for(lambda: not visible(dialog, IGNORE_CHECKSUM), "option hidden in create mode")
        close_patch_dialog(dialog)


def test_mismatched_source_is_rejected_unless_ignored(ported_exe_path, tmp_path):
    source, patch, expected = make_mismatched_inputs(tmp_path)
    result = tmp_path / "result.bin"
    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        fill_apply(dialog, source, patch, result)
        post_click(dialog, START)
        message, _title, items = wait_message_box(driver)
        assert any("元ファイルがパッチ作成時の内容と一致しません" in item for item in items)
        post_message_box_button(message, win32con.IDOK)
        wait_for(lambda: not win32gui.IsWindow(message), "error message box closed")
        assert not result.exists()

        click(dialog, IGNORE_CHECKSUM)
        post_click(dialog, START)
        message, _title, items = wait_message_box(driver)
        assert any("作成しましたが、元ファイルと結果ファイルのCRCが一致しません" in item
                   for item in items)
        post_message_box_button(message, win32con.IDOK)
        wait_for(lambda: not win32gui.IsWindow(message), "warning message box closed")
        assert text(dialog, STATUS).startswith("結果ファイルを作成しましたが")
        assert str(result) in text(dialog, STATUS)
        assert result.read_bytes() == expected
        close_patch_dialog(dialog)


def test_ignored_mismatch_warns_before_opening_result(ported_exe_path, tmp_path):
    source, patch, expected = make_mismatched_inputs(tmp_path)
    result = tmp_path / "result.bin"
    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        fill_apply(dialog, source, patch, result)
        click(dialog, OPEN_RESULT)
        click(dialog, IGNORE_CHECKSUM)
        post_click(dialog, START)
        message, _title, items = wait_message_box(driver)
        assert any("CRCが一致しません" in item for item in items)
        post_message_box_button(message, win32con.IDOK)
        wait_for(lambda: not win32gui.IsWindow(dialog), "apply dialog closed after opening result")
        wait_for(lambda: result.name in driver.get_mdi_child_titles(),
                 "applied result opened as a document")
        assert result.read_bytes() == expected
