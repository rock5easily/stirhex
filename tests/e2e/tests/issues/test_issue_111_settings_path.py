"""Issue #111: 使用中の設定ファイルのパスを環境設定「ファイル」ページで確認できる。

保存先は探索順（/ini: 指定 → 実行ファイル隣 → %APPDATA%）で決まるため、パスと併せて
「どの規則で決まったか」も表示する。ここでは既定の %APPDATA% と、/ini: で明示した場合の
両方を確認し、表示が探索順の分岐に追随していることを見る。

設定ファイルが読めなかった場合の注記（IDC_FILE_INI_READONLY）は、起動時のエラー
ダイアログがメインウィンドウより先に出るため自動操作では確認できない（Issue #96 で
確認済み）。手動確認に委ねる。
"""

from pathlib import Path

import pytest

from drivers.settings_context import settings_file_path
from drivers.stirling_driver import (
    IDC_FILE_INI_PATH,
    IDC_FILE_INI_READONLY,
    IDC_FILE_INI_SOURCE,
    StirlingDriver,
)


def _read_settings_group(drv) -> tuple[str, str, str]:
    """環境設定「ファイル」ページの「設定ファイル」グループを読む。"""
    sheet, page = drv.open_file_page()
    try:
        return (
            drv.read_control_text(page, IDC_FILE_INI_PATH),
            drv.read_control_text(page, IDC_FILE_INI_SOURCE),
            drv.read_control_text(page, IDC_FILE_INI_READONLY),
        )
    finally:
        drv.close_settings_sheet(sheet, accept=False)


@pytest.mark.ported
class TestIssue111SettingsPath:

    def test_default_location_is_shown_with_its_reason(self, ported_exe_path, sample_binary_file):
        """既定（%APPDATA%）で起動したとき、実際の保存先とその根拠が表示される。"""
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(sample_binary_file)
            path, source, note = _read_settings_group(drv)

        assert Path(path) == settings_file_path(), (
            f"表示されたパスが実際の保存先と違う: {path}"
        )
        assert "APPDATA" in source, f"保存先の根拠が %APPDATA% になっていない: {source}"
        assert note == "", f"正常に読めた起動では注記を出さない: {note}"

    def test_command_line_override_is_shown(self, ported_exe_path, sample_binary_file, tmp_path):
        """/ini: で指定したときは、そのパスと「コマンドライン指定」が表示される。"""
        ini = tmp_path / "portable.ini"
        ini.write_text("[Env]\n", encoding="utf-8")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(sample_binary_file, options=[f"/ini:{ini}"])
            path, source, _ = _read_settings_group(drv)

        assert Path(path) == ini, f"/ini: の指定が表示に反映されていない: {path}"
        assert "コマンドライン" in source, f"保存先の根拠が誤っている: {source}"
