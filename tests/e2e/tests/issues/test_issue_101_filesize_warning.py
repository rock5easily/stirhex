"""Issue #101: 大きいファイルを開く前の確認を、環境設定のしきい値で制御する。

512MB 固定だった確認（Issue #20）を、環境設定「ファイル」ページの
「大きいファイル」欄（有効/無効 + しきい値 MB）で制御できるようにした。

しきい値を小さくできるため、512MB のファイルを作らずに確認の有無を検証できる。
"""

import contextlib

import pytest
import win32con
import win32gui

from drivers.settings_context import settings_value
from drivers.stirling_driver import StirlingDriver, _set_control_text

PORT_ENV = r"Software\StirHex\StirHex\Env"

# しきい値の検証に使うサイズ。1MB のしきい値を確実に超え、かつ生成も読み込みも一瞬で済む。
SAMPLE_BYTES = 2 * 1024 * 1024


@contextlib.contextmanager
def _large_file_warning(enabled: bool, threshold_mb: int):
    """「大きいファイル」の設定を一時的に差し替える。"""
    with settings_value(PORT_ENV, "LargeFileWarn", 1 if enabled else 0):
        with settings_value(PORT_ENV, "LargeFileWarnMB", threshold_mb):
            yield


def _sample(tmp_path, name: str = "large.dat"):
    path = tmp_path / name
    path.write_bytes(b"\xA5" * SAMPLE_BYTES)
    return path


def _has_message_box(drv, timeout: float = 1.0) -> bool:
    """確認メッセージが出ているか。出ない場合はタイムアウトで False を返す。"""
    try:
        drv.find_message_box(timeout=timeout)
        return True
    except Exception:
        return False


@pytest.mark.ported
class TestIssue101FileSizeWarning:

    def test_file_at_or_above_threshold_asks_before_opening(self, ported_exe_path, tmp_path):
        """しきい値以上のファイルは確認を出し、拒否すると開かれない。"""
        target = _sample(tmp_path)

        with _large_file_warning(True, 1):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                drv.open_file_via_dialog(target)
                text = drv.answer_message_box(win32con.IDNO)
                assert "サイズが大きい" in text, f"確認メッセージが違う: {text}"

                titles = drv.get_mdi_child_titles()
                assert not any(target.name in t for t in titles), (
                    f"確認を拒否したのにファイルが開かれている: {titles}"
                )

    def test_file_below_threshold_opens_without_asking(self, ported_exe_path, tmp_path):
        """しきい値未満のファイルは確認せずに開く。"""
        target = _sample(tmp_path)

        with _large_file_warning(True, 64):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                drv.open_file_via_dialog(target)

                assert not _has_message_box(drv), "しきい値未満なのに確認が出た"
                titles = drv.get_mdi_child_titles()
                assert any(target.name in t for t in titles), f"開かれていない: {titles}"

    def test_warning_can_be_turned_off(self, ported_exe_path, tmp_path):
        """確認を無効にすると、しきい値を超えていても確認しない。"""
        target = _sample(tmp_path)

        with _large_file_warning(False, 1):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                drv.open_file_via_dialog(target)

                assert not _has_message_box(drv), "確認を無効にしたのに確認が出た"
                titles = drv.get_mdi_child_titles()
                assert any(target.name in t for t in titles), f"開かれていない: {titles}"

    def test_setting_round_trips_through_the_dialog(self, ported_exe_path, tmp_path):
        """環境設定で変えた値が保存され、次の起動で確認の挙動に効く。"""
        target = _sample(tmp_path)

        with _large_file_warning(True, 64):
            # 環境設定でしきい値を 1MB に下げて OK で確定する。
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                sheet, page = drv.open_file_page()
                edit = win32gui.GetDlgItem(page, 1157)   # IDC_FILE_LARGE_MB
                assert edit, "しきい値の入力欄が見つからない"
                _set_control_text(edit, "1")
                drv.close_settings_sheet(sheet, accept=True)

            # 次の起動で、下げたしきい値が効いていること。
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                drv.open_file_via_dialog(target)
                text = drv.answer_message_box(win32con.IDNO)
                assert "サイズが大きい" in text, f"確認メッセージが違う: {text}"
