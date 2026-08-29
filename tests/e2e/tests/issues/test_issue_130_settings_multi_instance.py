"""Issue #130: 複数インスタンスが同じ設定ファイルを使っても更新を失わない。

各プロセスが起動時に読み込んだ設定ストア全体を終了時に書き戻していたため、後から
終了したプロセスが、先に保存された更新を古いスナップショットの値で上書きしていた。
保存はプロセス間で直列化し、最新のファイル内容へ自分の変更だけを適用する。

保存側のマージ規則そのものはコア機能テスト（TestSettingsStoreChangeLog /
TestSettingsFileMergedSave / TestSettingsFileConcurrentSave）が持つ。ここでは実際に
2インスタンスを起動して、互いの設定変更が残ることを確認する。
"""

import contextlib
import time

import pytest
import win32gui

from drivers.settings_context import read_reg_values, settings_value
from drivers.stirling_driver import (
    IDC_ED1_SUBCARET,
    IDC_ED2_MARK_AUTO_RESTORE,
    StirlingDriver,
)

PORT_ENV = r"Software\StirHex\StirHex\Env"

BM_GETCHECK = 0x00F0
BM_SETCHECK = 0x00F1


@contextlib.contextmanager
def _multi_instance():
    with settings_value(PORT_ENV, "AllowMultipleInstances", 1):
        yield


def _set_check(drv, opener, control_id, checked):
    sheet, page = opener()
    ctrl = win32gui.GetDlgItem(page, control_id)
    assert ctrl, "control %d not found" % control_id
    win32gui.SendMessage(ctrl, BM_SETCHECK, 1 if checked else 0, 0)
    drv.close_settings_sheet(sheet, accept=True)


def _env_value(name):
    entry = read_reg_values(PORT_ENV).get(name)
    return None if entry is None else str(entry[0])


@pytest.mark.ported
class TestIssue130SettingsMultiInstance:

    def test_second_instance_does_not_revert_the_first(self, ported_exe_path, tmp_path):
        """後から終了したインスタンスが、先に保存された別キーの更新を消さないこと。"""
        target = tmp_path / "multi.dat"
        target.write_bytes(bytes(range(64)))

        with _multi_instance():
            with settings_value(PORT_ENV, "MarkAutoRestore", 0), \
                 settings_value(PORT_ENV, "SubCaret", 0):
                first = StirlingDriver(ported_exe_path)
                second = StirlingDriver(ported_exe_path)
                try:
                    first.start(target)
                    # 2つ目は1つ目と同じ時点のスナップショットを読んで起動する。
                    second.start()
                    assert first.pid != second.pid, "多重起動できていない"

                    # 1つ目で「マークの自動復元」を ON にして先に終了する。
                    _set_check(first, first.open_edit2_page,
                               IDC_ED2_MARK_AUTO_RESTORE, True)
                    first.close()
                    time.sleep(0.5)
                    assert _env_value("MarkAutoRestore") == "1", (
                        "1つ目の変更が保存されていない（前提が崩れている）"
                    )

                    # 2つ目は古いスナップショットのまま別のキーを変えて終了する。
                    _set_check(second, second.open_edit1_page, IDC_ED1_SUBCARET, True)
                    second.close()
                    time.sleep(0.5)
                finally:
                    first.close()
                    second.close()

                assert _env_value("SubCaret") == "1", "2つ目の変更が保存されていない"
                assert _env_value("MarkAutoRestore") == "1", (
                    "2つ目の終了で1つ目の更新が古い値へ戻されている"
                )
