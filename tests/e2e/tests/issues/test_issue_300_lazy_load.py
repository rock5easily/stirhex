"""Issue #300: 大きいファイルを全体読み込みせずに開き、編集・保存する（遅延読み込み）。

環境設定「ファイル」ページの「必要な部分だけ読み込む」（有効/無効 + しきい値 MB）以上の
ファイルは、元ファイルの区間を参照するだけの構造で開き、編集した位置だけを読み込む。
開いている間は参照先の内容を守るため、排他制御の設定によらず他プロセスの書込を拒否する。

しきい値を 1MB に下げ、数 MB のファイルで検証する（GB 級のファイルを作らずに済む）。
遅延読み込みで開いたかどうかは「排他制御が "しない" なのに書込用に開けない」ことで判定する
（全体読み込みでは排他制御 "しない" のときファイルを掴まない）。
"""

import contextlib
import os
from pathlib import Path

import pytest
import win32gui

from drivers import bgrep
from drivers.settings_context import settings_value
from drivers.stirling_driver import CMD_FILE_SAVE, StirlingDriver, _set_control_text

PORT_ENV = r"Software\StirHex\StirHex\Env"

SAMPLE_BYTES = 3 * 1024 * 1024 + 1234   # 1MB のしきい値を超え、16KB・64KB の境界に揃わない
EDIT_POS = 0x100003                     # 上書きする位置（参照ノードの内側）
INSERT_POS = 0x2001                     # 挿入する位置
MARKER = b"\xDE\xC0\xAD\x0B\xEE\x51"
MARKER_POS = 0x200000 + 77

IDC_FILE_LAZY_MB = 1167


@contextlib.contextmanager
def _lazy_settings(enabled: bool = True, threshold_mb: int = 1):
    """遅延読み込みの設定と、判定の前提（排他制御しない・大容量確認あり）を差し替える。"""
    with settings_value(PORT_ENV, "LazyLoad", 1 if enabled else 0), \
            settings_value(PORT_ENV, "LazyLoadMB", threshold_mb), \
            settings_value(PORT_ENV, "ExclusiveControl", 0), \
            settings_value(PORT_ENV, "LargeFileWarn", 1), \
            settings_value(PORT_ENV, "LargeFileWarnMB", 1), \
            settings_value(PORT_ENV, "BackupCreate", 0):
        yield


def _sample(tmp_path: Path, name: str = "lazy.dat") -> tuple[Path, bytes]:
    data = bytearray((i * 7 + (i >> 12)) & 0xFF for i in range(SAMPLE_BYTES))
    data[MARKER_POS:MARKER_POS + len(MARKER)] = MARKER
    path = tmp_path / name
    path.write_bytes(bytes(data))
    return path, bytes(data)


def _writable_by_others(path: Path) -> bool:
    """他プロセス（このテスト）が書込用に開けるか。"""
    try:
        with open(path, "r+b"):
            return True
    except PermissionError:
        return False


def _has_message_box(drv: StirlingDriver, timeout: float = 1.0) -> bool:
    try:
        drv.find_message_box(timeout=timeout)
        return True
    except Exception:
        return False


def _is_dirty(drv: StirlingDriver) -> bool:
    return any(title.endswith("*") for title in drv.get_mdi_child_titles())


def _save(drv: StirlingDriver) -> None:
    drv.post_command(CMD_FILE_SAVE)
    drv.wait_idle()
    assert not _is_dirty(drv), "保存後も変更マークが残っている"


@pytest.mark.ported
class TestIssue300LazyLoad:

    def test_opens_without_loading_and_saves_in_place(self, ported_exe_path, tmp_path):
        """しきい値以上は確認を出さずに遅延読み込みで開き、上書きは元ファイルへ直接書き戻す。"""
        target, data = _sample(tmp_path)
        inode = os.stat(target).st_ino

        with _lazy_settings():
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert not _has_message_box(drv), "遅延読み込みなのに大容量の確認が出た"
                assert any(target.name in t for t in drv.get_mdi_child_titles())
                assert not _writable_by_others(target), "開いている間は他プロセスの書込を拒否する"

                drv.jump_to_address(f"{EDIT_POS:X}", is_hex=True)
                drv.type_hex_chars("A5")   # 上書きモードで 1 バイト書き換え
                assert _is_dirty(drv)
                _save(drv)

                expected = bytearray(data)
                expected[EDIT_POS] = 0xA5
                assert target.read_bytes() == bytes(expected), "上書き保存の内容が違う"
                assert os.stat(target).st_ino == inode, (
                    "長さが変わらない編集なのに、ファイルが置き換えられた（上書き保存されていない）"
                )
                assert not _writable_by_others(target), "保存後もロックを保持する"

        assert _writable_by_others(target), "閉じたらロックを外す"

    def test_same_length_replace_is_saved_in_place(self, ported_exe_path, tmp_path):
        """同じ長さの全置換は位置をずらさないため、上書き保存の対象になる。"""
        target, data = _sample(tmp_path)
        inode = os.stat(target).st_ino
        replacement = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66])

        with _lazy_settings():
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                drv.replace_all_dialog(MARKER.hex().upper(), replacement.hex().upper())
                drv.wait_idle()
                assert _is_dirty(drv), "置換されていない"
                _save(drv)

                expected = data.replace(MARKER, replacement)
                assert expected != data
                assert target.read_bytes() == expected, "置換して保存した内容が違う"
                assert os.stat(target).st_ino == inode, "同じ長さの置換なのに上書き保存されていない"

                drv.undo()
                _save(drv)
                assert target.read_bytes() == data, "置換の Undo を保存すると元へ戻る"
                assert os.stat(target).st_ino == inode, "置換の Undo も上書き保存の対象になる"

    def test_insert_save_then_undo_and_save_again(self, ported_exe_path, tmp_path):
        """挿入した文書は一時ファイル方式で保存し、保存後の Undo と再保存も正しく反映される。"""
        target, data = _sample(tmp_path)
        inode = os.stat(target).st_ino

        with _lazy_settings():
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                drv.press_insert()                     # 挿入モードへ
                drv.jump_to_address(f"{INSERT_POS:X}", is_hex=True)
                drv.type_hex_chars("BEEF")             # 2 バイト挿入
                _save(drv)

                inserted = data[:INSERT_POS] + b"\xBE\xEF" + data[INSERT_POS:]
                assert target.read_bytes() == inserted, "挿入して保存した内容が違う"
                assert os.stat(target).st_ino != inode, "長さが変わる保存は一時ファイルから置き換える"
                assert not _writable_by_others(target), "保存したファイルを読み出し元として掴み直す"

                drv.undo()
                drv.undo()
                assert _is_dirty(drv), "保存後の Undo で変更ありになる"
                _save(drv)
                assert target.read_bytes() == data, "保存後に Undo して保存すると元へ戻る"

    def test_save_as_moves_the_lock_to_the_new_file(self, ported_exe_path, tmp_path):
        """別名保存すると保存先を読み出し元にし直し、元ファイルのロックを外す。"""
        source, data = _sample(tmp_path, "lazy_source.dat")
        destination = tmp_path / "lazy_destination.dat"

        with _lazy_settings():
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(source)
                drv.jump_to_address("0", is_hex=True)
                drv.type_hex_chars("11")
                assert drv.save_as_via_dialog(destination) is True

                assert destination.read_bytes() == b"\x11" + data[1:], "別名保存の内容が違う"
                assert source.read_bytes() == data, "元ファイルは変わらない"
                assert _writable_by_others(source), "元ファイルのロックは外れる"
                assert not _writable_by_others(destination), "保存先をロックする"

    def test_below_threshold_or_disabled_loads_whole_file(self, ported_exe_path, tmp_path):
        """しきい値未満、または無効なら従来どおり全体を読み込む（排他制御 "しない" ならロックしない）。"""
        target, _data = _sample(tmp_path)

        for enabled, threshold in ((True, 64), (False, 1)):
            with _lazy_settings(enabled, threshold):
                with StirlingDriver(ported_exe_path) as drv:
                    drv.start(target)
                    # 全体読み込みなので大容量の確認（しきい値 1MB）が出る。
                    text = drv.answer_message_box(6)   # IDYES
                    assert "サイズが大きい" in text, f"確認メッセージが違う: {text}"
                    drv.wait_idle()
                    assert _writable_by_others(target), (
                        f"全体読み込み（有効={enabled}, しきい値={threshold}MB）でロックしている"
                    )

    def test_threshold_round_trips_through_the_dialog(self, ported_exe_path, tmp_path):
        """環境設定で変えたしきい値が保存され、次の起動で効く。"""
        target, _data = _sample(tmp_path)

        with _lazy_settings(True, 64):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                sheet, page = drv.open_file_page()
                edit = win32gui.GetDlgItem(page, IDC_FILE_LAZY_MB)
                assert edit, "遅延読み込みのしきい値の入力欄が見つからない"
                _set_control_text(edit, "1")
                drv.close_settings_sheet(sheet, accept=True)

            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert not _has_message_box(drv), "下げたしきい値で遅延読み込みになっていない"
                assert not _writable_by_others(target), "遅延読み込みで開いていない"

    def test_bgrep_searches_without_loading(self, ported_exe_path, tmp_path):
        """BGREP はファイルを遅延読み込みで検索し、ヒット位置を正しく報告する。"""
        folder = tmp_path / "bgrep"
        folder.mkdir()
        _sample(folder, "hit.dat")

        with _lazy_settings():
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                lines, applied = bgrep.run(drv, folder, " ".join(f"{b:02X}" for b in MARKER))

        offsets = [line.rsplit(":", 1)[-1].strip().lower() for line in lines if ":" in line]
        assert f"{MARKER_POS:08x}" in offsets, f"ヒット位置が報告されない: {lines}（実効値: {applied}）"
