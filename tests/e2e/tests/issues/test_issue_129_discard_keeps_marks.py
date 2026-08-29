"""Issue #129: 未保存編集を破棄して閉じたときにマーク記録を壊さない。

マーク自動復元が ON の文書で挿入・削除を行い、「保存しない」で閉じると、破棄される
メモリ上の大きさとマークが `RecordMarks` へ渡されていた。ディスク上のファイルは元の
ままなので、次回の `LookupMarks` は大きさ不一致となり復元されず、それ以前の有効な
記録まで失われる。
"""

import contextlib
import time

import pytest
import win32con

from drivers.settings_context import read_reg_values, registry_section, settings_value
from drivers.stirling_driver import (
    CMD_FILE_CLOSE,
    CMD_FILE_SAVE,
    ID_MARK2_TOGGLE,
    StirlingDriver,
)

PORT_ENV = r"Software\StirHex\StirHex\Env"
PORT_MARK_STORE = r"Software\StirHex\StirHex\MarkStore"

SAMPLE = bytes(range(256)) * 4      # 1024 バイト
MARK_A = 0x10
MARK_B = 0x20


@contextlib.contextmanager
def _auto_restore(enabled: bool):
    with settings_value(PORT_ENV, "MarkAutoRestore", 1 if enabled else 0):
        yield


@pytest.fixture(autouse=True)
def _clean_mark_store():
    with registry_section(PORT_MARK_STORE):
        yield


def _mark_addresses(drv) -> list[int]:
    dialog = drv.open_mark_list_dialog()
    try:
        rows = drv.mark_list_entries(dialog)
    finally:
        drv.click_dialog_button(dialog, win32con.IDCANCEL)
    return sorted(int(text.split()[0], 16) for text, _data in rows)


def _put_marks(drv, path):
    drv.start(path)
    drv.jump_to_address("%X" % MARK_A)
    drv.mark_toggle()
    drv.jump_to_address("%X" % MARK_B)
    drv.post_command(ID_MARK2_TOGGLE)


def _insert_a_byte(drv):
    """挿入モードで1バイト挿入し、文書の大きさを変える。"""
    drv.jump_to_address("0")
    drv.press_insert()          # 上書き → 挿入
    drv.type_hex_chars("FF")


def _close_document(drv, save: bool):
    """文書だけを閉じる（アプリは残す）。save=False は「保存しない」を選ぶ。"""
    drv.post_command(CMD_FILE_CLOSE)
    drv.answer_message_box(win32con.IDYES if save else win32con.IDNO)


@pytest.mark.ported
class TestIssue129DiscardKeepsMarks:

    def test_discarded_edits_keep_the_previous_record(self, ported_exe_path, tmp_path):
        target = tmp_path / "discard.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                _put_marks(drv, target)
            recorded = read_reg_values(PORT_MARK_STORE)
            assert recorded, "マークが記録されていない（前提が崩れている）"

            # 大きさを変える編集をして「保存しない」で閉じる。
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert _mark_addresses(drv) == [MARK_A, MARK_B]
                _insert_a_byte(drv)
                _close_document(drv, save=False)

            assert target.read_bytes() == SAMPLE, "ディスク上のファイルが変わっている"
            assert read_reg_values(PORT_MARK_STORE) == recorded, (
                "破棄された編集内容でマーク記録が上書きされている"
            )

            # 以前の有効な記録がそのまま使えること。
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                addresses = _mark_addresses(drv)
        assert addresses == [MARK_A, MARK_B], (
            "破棄後にマークが復元されない: %s" % addresses
        )

    def test_saving_the_edit_records_the_new_state(self, ported_exe_path, tmp_path):
        """保存して閉じる場合は、保存後の大きさとマークで記録する（既存動作の維持）。"""
        target = tmp_path / "saved.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                _put_marks(drv, target)

            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                _insert_a_byte(drv)
                drv.post_command(CMD_FILE_SAVE)
                time.sleep(0.5)

            assert len(target.read_bytes()) == len(SAMPLE) + 1, "保存されていない"

            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                addresses = _mark_addresses(drv)
        assert addresses, "保存して閉じた場合にマークが復元されない: %s" % addresses

    def test_unmodified_close_still_records(self, ported_exe_path, tmp_path):
        """未変更で閉じる場合は従来どおり記録する（既存動作の維持）。"""
        target = tmp_path / "unmodified.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                drv.jump_to_address("%X" % MARK_A)
                drv.mark_toggle()
                # 未変更なので、そのまま終了しても確認は出ない。

            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                addresses = _mark_addresses(drv)
        assert addresses == [MARK_A], "未変更で閉じたマークが記録されていない: %s" % addresses
