"""Issue #259: create/apply disk-file IPS/BPS patches from the main menu."""

import os
import struct
import time
import zlib
from pathlib import Path

import pytest
import win32con
import win32gui
from pywinauto import timings

from drivers.stirling_driver import StirlingDriver, _control_text, _set_control_text

pytestmark = pytest.mark.ported

CREATE_COMMAND = 33021
APPLY_COMMAND = 33022
MODE_CREATE = 1500
MODE_APPLY = 1501
SOURCE_EDIT = 1502
SECOND_EDIT = 1504
RESULT_EDIT = 1506
FORMAT_BPS = 1508
FORMAT_IPS = 1509
STATUS = 1510
START = 1512
OPEN_RESULT = 1518
FORMAT_NOTE = 1519


def wait_for(predicate, description, timeout=10):
    def check():
        value = predicate()
        assert value, description
        return value

    return timings.wait_until_passes(timeout, 0.05, check)


def text(dialog, control):
    return _control_text(win32gui.GetDlgItem(dialog, control))


def set_text(dialog, control, value):
    _set_control_text(win32gui.GetDlgItem(dialog, control), str(value))


def click(dialog, control):
    win32gui.SendMessage(win32gui.GetDlgItem(dialog, control), win32con.BM_CLICK, 0, 0)


def post_click(dialog, control):
    """Click asynchronously when the handler may open a cross-process modal box."""
    button = win32gui.GetDlgItem(dialog, control)
    win32gui.PostMessage(button, win32con.BM_CLICK, 0, 0)


def open_patch_dialog(driver, command):
    driver.post_command(command)
    return driver.find_process_dialog("バイナリパッチ")


def close_patch_dialog(dialog):
    win32gui.PostMessage(dialog, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
    wait_for(lambda: not win32gui.IsWindow(dialog), "binary patch dialog closed")


def write_ips(path, offset, data):
    """Write one basic IPS record; no extended-size or RLE records are needed here."""
    path.write_bytes(
        b"PATCH"
        + int(offset).to_bytes(3, "big")
        + len(data).to_bytes(2, "big")
        + data
        + b"EOF"
    )


def write_bps_one_byte_change(path, source, offset, value):
    """Write a BPS patch that replaces one byte of source (SourceRead/TargetRead/SourceRead).

    The patch is built here from the BPS format so the test does not depend on
    StirHex's own encoder; offset must leave at least one byte on either side.
    """
    def vli(number):
        out = bytearray()
        while True:
            part = number & 0x7F
            number >>= 7
            if number == 0:
                out.append(part | 0x80)
                return bytes(out)
            out.append(part)
            number -= 1

    target = source[:offset] + bytes([value]) + source[offset + 1:]
    body = b"BPS1" + vli(len(source)) + vli(len(target)) + vli(0)
    body += vli((offset - 1) << 2)                     # SourceRead offset bytes
    body += vli(1) + bytes([value])                    # TargetRead one byte
    body += vli((len(source) - offset - 2) << 2)       # SourceRead the rest
    body += struct.pack("<II", zlib.crc32(source), zlib.crc32(target))
    body += struct.pack("<I", zlib.crc32(body))
    path.write_bytes(body)
    return target


def wait_message_box(driver, timeout=5):
    def find():
        for hwnd, cls, title in driver._get_process_windows():
            if cls != "#32770" or title == "バイナリパッチ":
                continue
            items = []

            def collect(child, _):
                items.append(_control_text(child))
                return True

            win32gui.EnumChildWindows(hwnd, collect, None)
            return hwnd, title, items
        raise RuntimeError("message box not found")

    return timings.wait_until_passes(timeout, 0.05, find)


def post_message_box_button(dialog, button_id):
    buttons = []

    def collect(child, _):
        if win32gui.GetClassName(child) == "Button":
            buttons.append((child, win32gui.GetDlgCtrlID(child), _control_text(child)))
        return True

    win32gui.EnumChildWindows(dialog, collect, None)
    button = next((h for h, cid, _ in buttons if cid == button_id), None)
    if button is None:
        labels = {win32con.IDOK: {"OK", "はい", "Yes"},
                  win32con.IDYES: {"はい", "Yes"},
                  win32con.IDNO: {"いいえ", "No"}}.get(button_id, set())
        button = next((h for h, _, label in buttons if label in labels), None)
    if button is None:
        raise RuntimeError(f"message box button not found: {buttons!r}")
    win32gui.PostMessage(button, win32con.BM_CLICK, 0, 0)


def fill_apply(dialog, source, patch, result):
    set_text(dialog, SOURCE_EDIT, source)
    set_text(dialog, SECOND_EDIT, patch)
    set_text(dialog, RESULT_EDIT, result)


def test_required_paths_control_start_and_open_option_visibility(ported_exe_path, tmp_path):
    with StirlingDriver(ported_exe_path) as driver:
        driver.start()

        dialog = open_patch_dialog(driver, CREATE_COMMAND)
        assert not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, START))
        assert not win32gui.IsWindowVisible(win32gui.GetDlgItem(dialog, OPEN_RESULT))
        set_text(dialog, SOURCE_EDIT, tmp_path / "source.bin")
        set_text(dialog, SECOND_EDIT, tmp_path / "target.bin")
        set_text(dialog, RESULT_EDIT, tmp_path / "patch.bps")
        wait_for(lambda: win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, START)),
                 "create button enabled after required paths are entered")
        close_patch_dialog(dialog)

        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        assert not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, START))
        assert win32gui.IsWindowVisible(win32gui.GetDlgItem(dialog, OPEN_RESULT))
        fill_apply(dialog, tmp_path / "source.bin", tmp_path / "patch.bps",
                    tmp_path / "result.bin")
        wait_for(lambda: win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, START)),
                 "apply button enabled after required paths are entered")
        close_patch_dialog(dialog)


def test_error_status_does_not_overlap_open_result_option(ported_exe_path, tmp_path):
    source = tmp_path / "source.bin"
    patch = tmp_path / "invalid.bps"
    result = tmp_path / "result.bin"
    source.write_bytes(b"source")
    patch.write_bytes(b"not a valid patch")

    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        fill_apply(dialog, source, patch, result)
        post_click(dialog, START)
        message, _title, _items = wait_message_box(driver)
        status_rect = win32gui.GetWindowRect(win32gui.GetDlgItem(dialog, STATUS))
        open_handle = win32gui.GetDlgItem(dialog, OPEN_RESULT)
        open_rect = win32gui.GetWindowRect(open_handle)
        assert win32gui.IsWindowVisible(open_handle)
        assert open_rect[1] >= status_rect[3]
        post_message_box_button(message, win32con.IDOK)
        wait_for(lambda: not win32gui.IsWindow(message), "error message box closed")
        close_patch_dialog(dialog)


def test_create_and_apply_bps_without_document(ported_exe_path, tmp_path):
    source = tmp_path / "before.bin"
    target = tmp_path / "after.bin"
    patch = tmp_path / "change.bps"
    result = tmp_path / "result.bin"
    source.write_bytes(bytes(range(32)) * 32)
    target.write_bytes(bytes(range(16, 32)) * 64)

    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, CREATE_COMMAND)
        assert win32gui.SendMessage(win32gui.GetDlgItem(dialog, MODE_CREATE),
                                    win32con.BM_GETCHECK, 0, 0) == win32con.BST_CHECKED
        assert win32gui.SendMessage(win32gui.GetDlgItem(dialog, FORMAT_BPS),
                                    win32con.BM_GETCHECK, 0, 0) == win32con.BST_CHECKED
        set_text(dialog, SOURCE_EDIT, source)
        set_text(dialog, SECOND_EDIT, target)
        set_text(dialog, RESULT_EDIT, patch)
        post_click(dialog, START)
        wait_for(lambda: text(dialog, STATUS).startswith("パッチを作成しました。"),
                 "BPS patch created")
        assert patch.read_bytes().startswith(b"BPS1")
        close_patch_dialog(dialog)

        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        assert win32gui.SendMessage(win32gui.GetDlgItem(dialog, MODE_APPLY),
                                    win32con.BM_GETCHECK, 0, 0) == win32con.BST_CHECKED
        set_text(dialog, SOURCE_EDIT, source)
        set_text(dialog, SECOND_EDIT, patch)
        set_text(dialog, RESULT_EDIT, result)
        click(dialog, OPEN_RESULT)
        post_click(dialog, START)
        wait_for(lambda: not win32gui.IsWindow(dialog), "apply dialog closed after opening result")
        wait_for(lambda: result.name in driver.get_mdi_child_titles(),
                 "applied result opened as a document")
        assert result.read_bytes() == target.read_bytes()


def test_ips_is_disabled_when_generation_policy_does_not_match(ported_exe_path, tmp_path):
    source = tmp_path / "source.bin"
    target = tmp_path / "target.bin"
    source.write_bytes(b"a")
    target.write_bytes(b"ab")
    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, CREATE_COMMAND)
        set_text(dialog, SOURCE_EDIT, source)
        set_text(dialog, SECOND_EDIT, target)
        wait_for(lambda: not win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, FORMAT_IPS)),
                 "IPS disabled for different-sized files")
        assert "16MiB" in text(dialog, STATUS)
        close_patch_dialog(dialog)


@pytest.mark.parametrize(("size", "enabled"), [
    (16 * 1024 * 1024, True),
    (16 * 1024 * 1024 + 1, False),
])
def test_ips_generation_size_boundary(ported_exe_path, tmp_path, size, enabled):
    source = tmp_path / f"ips_{size}_source.bin"
    target = tmp_path / f"ips_{size}_target.bin"
    source.write_bytes(b"A" * size)
    target.write_bytes(b"B" * size)
    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, CREATE_COMMAND)
        set_text(dialog, SOURCE_EDIT, source)
        set_text(dialog, SECOND_EDIT, target)
        wait_for(lambda: bool(win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, FORMAT_IPS))) == enabled,
                 f"IPS boundary enabled={enabled}")
        close_patch_dialog(dialog)


def test_apply_non_ascii_paths_and_existing_output_is_preserved(ported_exe_path, tmp_path):
    directory = tmp_path / "日本語 パッチ"
    directory.mkdir()
    source = directory / "元.bin"
    patch = directory / "変更.ips"
    result = directory / "結果.bin"
    source.write_bytes(b"0123456789")
    write_ips(patch, 0, b"AB")
    result.write_bytes(b"keep-me")

    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        fill_apply(dialog, source, patch, result)
        post_click(dialog, START)
        message, _title, items = wait_message_box(driver)
        assert message
        assert any("既に存在します" in item for item in items)
        post_message_box_button(message, win32con.IDNO)
        wait_for(lambda: win32gui.IsWindow(dialog), "apply dialog remains after declining overwrite")
        assert result.read_bytes() == b"keep-me"
        close_patch_dialog(dialog)

        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        result.unlink()
        fill_apply(dialog, source, patch, result)
        click(dialog, START)
        wait_for(lambda: text(dialog, STATUS).startswith("結果ファイルを作成しました。"),
                 "non-ASCII result created")
        assert result.read_bytes() == b"AB23456789"
        close_patch_dialog(dialog)


def confirm_overwrite(driver, dialog):
    """Start the operation, answer Yes to the overwrite prompt, and wait for it to close."""
    post_click(dialog, START)
    message, _title, items = wait_message_box(driver)
    assert any("既に存在します" in item for item in items)
    post_message_box_button(message, win32con.IDYES)
    wait_for(lambda: not win32gui.IsWindow(message), "overwrite confirmation closed")


def test_apply_overwrite_confirmed_replaces_existing_result(ported_exe_path, tmp_path):
    """Issue #267: answering Yes replaces the existing result without leftovers."""
    source = tmp_path / "overwrite-source.bin"
    patch = tmp_path / "overwrite-source.ips"
    result = tmp_path / "overwrite-result.bin"
    source.write_bytes(b"0123456789")
    write_ips(patch, 0, b"AB")
    result.write_bytes(b"old result that is longer than the new one")
    entries = sorted(p.name for p in tmp_path.iterdir())

    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        fill_apply(dialog, source, patch, result)
        confirm_overwrite(driver, dialog)
        wait_for(lambda: text(dialog, STATUS).startswith("結果ファイルを作成しました。"),
                 "existing result replaced")
        close_patch_dialog(dialog)

    assert result.read_bytes() == b"AB23456789"
    assert source.read_bytes() == b"0123456789"
    assert sorted(p.name for p in tmp_path.iterdir()) == entries


def test_create_overwrite_confirmed_replaces_existing_patch(ported_exe_path, tmp_path):
    """Issue #267: answering Yes replaces an existing patch file with the new BPS."""
    source = tmp_path / "overwrite-before.bin"
    target = tmp_path / "overwrite-after.bin"
    patch = tmp_path / "overwrite.bps"
    applied = tmp_path / "overwrite-applied.bin"
    source.write_bytes(bytes(range(64)) * 4)
    target.write_bytes(bytes(range(64)) * 2 + b"INSERTED" + bytes(range(64)) * 2)
    patch.write_bytes(b"old patch contents")
    entries = sorted(p.name for p in tmp_path.iterdir())

    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, CREATE_COMMAND)
        set_text(dialog, SOURCE_EDIT, source)
        set_text(dialog, SECOND_EDIT, target)
        set_text(dialog, RESULT_EDIT, patch)
        confirm_overwrite(driver, dialog)
        wait_for(lambda: text(dialog, STATUS).startswith("パッチを作成しました。"),
                 "existing patch replaced")
        close_patch_dialog(dialog)
        assert sorted(p.name for p in tmp_path.iterdir()) == entries
        assert patch.read_bytes().startswith(b"BPS1")

        # The replaced patch is a working patch, not a mix of old and new bytes.
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        fill_apply(dialog, source, patch, applied)
        click(dialog, START)
        wait_for(lambda: text(dialog, STATUS).startswith("結果ファイルを作成しました。"),
                 "replaced patch applies")
        close_patch_dialog(dialog)
    assert applied.read_bytes() == target.read_bytes()


def test_output_hardlink_to_open_document_is_rejected(ported_exe_path, tmp_path):
    source = tmp_path / "open-source.bin"
    patch = tmp_path / "open-source.ips"
    other = tmp_path / "other-open-document.bin"
    hardlink = tmp_path / "other-open-result.bin"
    source.write_bytes(b"0123456789")
    other.write_bytes(b"abcdefghij")
    write_ips(patch, 0, b"AB")
    os.link(other, hardlink)

    with StirlingDriver(ported_exe_path) as driver:
        driver.start(source)
        driver.open_file_via_dialog(other)
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        fill_apply(dialog, source, patch, hardlink)
        post_click(dialog, START)
        message, _title, items = wait_message_box(driver)
        assert any("入力ファイル" in item and "開いている文書" in item for item in items)
        post_message_box_button(message, win32con.IDOK)
        wait_for(lambda: not win32gui.IsWindow(message), "hardlink conflict message closed")
        assert source.read_bytes() == b"0123456789"
        assert other.read_bytes() == b"abcdefghij"
        assert hardlink.read_bytes() == b"abcdefghij"
        close_patch_dialog(dialog)


def test_unsaved_edit_undo_and_marks_are_not_touched_by_apply(ported_exe_path, tmp_path):
    source = tmp_path / "dirty-source.bin"
    patch = tmp_path / "dirty-source.ips"
    result = tmp_path / "dirty-result.bin"
    source.write_bytes(b"0123456789")
    write_ips(patch, 0, b"AB")

    with StirlingDriver(ported_exe_path) as driver:
        driver.start(source)
        driver.type_hex_chars("FF")
        assert any(title.endswith("*") for title in driver.get_mdi_child_titles())
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        fill_apply(dialog, source, patch, result)
        click(dialog, START)
        wait_for(lambda: text(dialog, STATUS).startswith("結果ファイルを作成しました。"),
                 "apply result from disk bytes")
        close_patch_dialog(dialog)
        assert result.read_bytes() == b"AB23456789"
        assert source.read_bytes() == b"0123456789"
        assert any(title.endswith("*") for title in driver.get_mdi_child_titles())
        driver.undo()
        assert all(not title.endswith("*") for title in driver.get_mdi_child_titles())
        assert source.read_bytes() == b"0123456789"


def test_apply_can_be_cancelled_before_publish(ported_exe_path, tmp_path):
    source = tmp_path / "cancel-source.bin"
    patch = tmp_path / "cancel-source.ips"
    source.write_bytes(b"A" * (128 * 1024 * 1024))
    write_ips(patch, 0, b"B")

    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        # The operation is intentionally retried at most three times if a fast machine
        # finishes before the first timer turn; this is below the ten-attempt GUI cap.
        for attempt in range(3):
            result = tmp_path / f"cancel-result-{attempt}.bin"
            fill_apply(dialog, source, patch, result)
            click(dialog, START)
            deadline = time.time() + 15
            cancelled = False
            while time.time() < deadline:
                if win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, 1513)):
                    click(dialog, 1513)
                    wait_for(lambda: text(dialog, STATUS).startswith("処理を中止しました。"),
                             "patch cancellation")
                    cancelled = True
                    break
                if text(dialog, STATUS).startswith("結果ファイルを作成しました。"):
                    break
                time.sleep(0.02)
            if cancelled:
                assert not result.exists()
                break
        else:
            pytest.fail("patch completed before cancellation in all attempts")
        close_patch_dialog(dialog)


IPS_UNVERIFIED_NOTE = "IPS は元ファイルの内容を検証しません。内容を確認してから適用してください。"
BPS_VERIFIED_NOTE = "BPS は元ファイルのCRCを検証してから適用します。"
AUTO_FORMAT_NOTE = "自動判別（IPS/BPS）"
APPLY_OUTPUT_CONFLICT_MESSAGE = "結果ファイルは入力ファイル、パッチファイル、または開いている文書と別のファイルを指定してください。"
# Issue #272: the create mode names its own output field ("パッチファイル").
CREATE_OUTPUT_CONFLICT_MESSAGE = "パッチファイルは元ファイル、変更後ファイル、または開いている文書と別のファイルを指定してください。"


def test_create_and_apply_ips_from_the_dialog(ported_exe_path, tmp_path):
    """Issue #269: IPS is created from the dialog and the apply side warns it is unverified."""
    source = tmp_path / "ips-before.bin"
    target = tmp_path / "ips-after.bin"
    patch = tmp_path / "change.ips"
    result = tmp_path / "ips-result.bin"
    source.write_bytes(bytes(range(256)))
    changed = bytearray(range(256))
    changed[16:20] = b"IPS!"
    changed[200:232] = bytes([0x5A]) * 32
    target.write_bytes(bytes(changed))

    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, CREATE_COMMAND)
        set_text(dialog, SOURCE_EDIT, source)
        set_text(dialog, SECOND_EDIT, target)
        set_text(dialog, RESULT_EDIT, patch)
        wait_for(lambda: win32gui.IsWindowEnabled(win32gui.GetDlgItem(dialog, FORMAT_IPS)),
                 "IPS enabled for same-sized files")
        click(dialog, FORMAT_IPS)
        post_click(dialog, START)
        wait_for(lambda: text(dialog, STATUS).startswith("パッチを作成しました。"),
                 "IPS patch created")
        close_patch_dialog(dialog)
        data = patch.read_bytes()
        assert data.startswith(b"PATCH") and data.endswith(b"EOF")

        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        assert text(dialog, FORMAT_NOTE) == AUTO_FORMAT_NOTE
        fill_apply(dialog, source, patch, result)
        wait_for(lambda: text(dialog, FORMAT_NOTE) == IPS_UNVERIFIED_NOTE,
                 "IPS patch shows the unverified-source note")
        post_click(dialog, START)
        wait_for(lambda: text(dialog, STATUS).startswith("結果ファイルを作成しました。"),
                 "IPS patch applied")
        close_patch_dialog(dialog)
    assert result.read_bytes() == target.read_bytes()


@pytest.mark.parametrize(("case", "reason"), [
    ("source_mismatch", "元ファイルがパッチ作成時の内容と一致しません。"),
    ("invalid_patch", "パッチ形式が不正です。"),
], ids=["source_mismatch", "invalid_patch"])
def test_apply_failure_reports_the_reason(ported_exe_path, tmp_path, case, reason):
    """Issue #269: each failure status is shown with its own reason and publishes nothing."""
    source = tmp_path / "reason-source.bin"
    patch = tmp_path / "reason.bps"
    result = tmp_path / "reason-result.bin"
    original = b"0123456789abcdef"
    write_bps_one_byte_change(patch, original, 5, ord("X"))
    if case == "source_mismatch":
        source.write_bytes(b"0123456789abcdeF")  # same size, different content
    else:
        source.write_bytes(original)
        data = bytearray(patch.read_bytes())
        data[-1] ^= 0x01  # patch CRC
        patch.write_bytes(bytes(data))
    entries = sorted(p.name for p in tmp_path.iterdir())

    with StirlingDriver(ported_exe_path) as driver:
        driver.start()
        dialog = open_patch_dialog(driver, APPLY_COMMAND)
        fill_apply(dialog, source, patch, result)
        wait_for(lambda: text(dialog, FORMAT_NOTE) == BPS_VERIFIED_NOTE,
                 "BPS patch shows the CRC verification note")
        post_click(dialog, START)
        message, _title, items = wait_message_box(driver)
        assert any(reason in item for item in items), items
        post_message_box_button(message, win32con.IDOK)
        wait_for(lambda: not win32gui.IsWindow(message), "failure message closed")
        assert reason in text(dialog, STATUS)
        close_patch_dialog(dialog)
    assert not result.exists()
    assert sorted(p.name for p in tmp_path.iterdir()) == entries


@pytest.mark.parametrize("conflict", ["source", "target_hardlink", "open_document"])
def test_create_rejects_output_that_is_an_input(ported_exe_path, tmp_path, conflict):
    """Issues #269/#272: the patch path cannot be an input or an open document.

    The message names the create mode's own fields (patch file, source, target).
    """
    source = tmp_path / "conflict-before.bin"
    target = tmp_path / "conflict-after.bin"
    source.write_bytes(b"before contents")
    target.write_bytes(b"after contents!")
    open_files = []
    if conflict == "source":
        output = source
    elif conflict == "target_hardlink":
        output = tmp_path / "conflict-after-link.bps"
        os.link(target, output)
    else:
        output = tmp_path / "conflict-open-document.bps"
        output.write_bytes(b"open document")
        open_files.append(output)
    entries = sorted(p.name for p in tmp_path.iterdir())

    with StirlingDriver(ported_exe_path) as driver:
        driver.start(*open_files)
        dialog = open_patch_dialog(driver, CREATE_COMMAND)
        set_text(dialog, SOURCE_EDIT, source)
        set_text(dialog, SECOND_EDIT, target)
        set_text(dialog, RESULT_EDIT, output)
        post_click(dialog, START)
        message, _title, items = wait_message_box(driver)
        assert any(CREATE_OUTPUT_CONFLICT_MESSAGE in item for item in items), items
        assert not any(APPLY_OUTPUT_CONFLICT_MESSAGE in item for item in items), items
        post_message_box_button(message, win32con.IDOK)
        wait_for(lambda: not win32gui.IsWindow(message), "conflict message closed")
        close_patch_dialog(dialog)
    assert source.read_bytes() == b"before contents"
    assert target.read_bytes() == b"after contents!"
    if conflict == "open_document":
        assert output.read_bytes() == b"open document"
    assert sorted(p.name for p in tmp_path.iterdir()) == entries
