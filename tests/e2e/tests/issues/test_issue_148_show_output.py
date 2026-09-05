"""Issue #148: アウトプットペインの表示状態を保存／復元する。

原版は設定値 `ShowOutput` を終了時に書き出すものの、起動時に読み戻すだけで
`CMainFrame::OnCreate`（FUN_0041e8a1）が一切参照しない。そのため `ShowOutput=1` でも
アウトプットペインは常に非表示で開始する（実質デッド設定）。

移植版では phase5 の独自機能として、保存だけでなく起動時の復元まで行う。
ここでは原版の「復元しない」挙動と、移植版の「復元する」挙動を突き合わせる。
"""

import time

import pytest

from drivers.settings_context import read_reg_values, settings_value
from drivers.stirling_driver import StirlingDriver

ORIG_SETTINGS = r"Software\DDS2\Stirling\Settings"
PORT_ENV = r"Software\StirHex\StirHex\Env"

REG_DWORD = 4


def _wait_for_show_output(expected: int, timeout: float = 10.0):
    """終了処理が設定を書き終えるまで待って ShowOutput を突き合わせる。

    設定の保存は ExitInstance で行われるため、プロセスの終了要求からファイルへ
    書き終わるまでに間がある。固定の sleep では取りこぼすので、期待値が現れるまで
    待つ。現れないまま時間切れになった場合は、その時点の値を添えて失敗させる。
    """
    deadline = time.time() + timeout
    while True:
        value = read_reg_values(PORT_ENV).get("ShowOutput")
        if value == (expected, REG_DWORD):
            return
        if time.time() >= deadline:
            pytest.fail(f"ShowOutput が {expected} にならなかった（現在値: {value}）")
        time.sleep(0.1)


@pytest.mark.original
def test_original_does_not_restore_output_pane(original_exe_path, tmp_path):
    """原版は ShowOutput=1 でもアウトプットペインを復元しない（移植漏れではない証跡）。"""
    test_file = tmp_path / "show_output_orig.dat"
    test_file.write_bytes(b"\x00" * 256)

    with settings_value(ORIG_SETTINGS, "ShowOutput", 1):
        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert drv.find_output_bar() is not None, "アウトプットバーが見つからない"
            assert not drv.is_output_pane_visible(), (
                "原版は ShowOutput=1 でも起動時にアウトプットペインを表示しないはず"
            )


@pytest.mark.ported
def test_ported_restores_output_pane(ported_exe_path, tmp_path):
    """移植版は ShowOutput の表示状態を終了時に保存し、次回起動で復元する。"""
    test_file = tmp_path / "show_output_port.dat"
    test_file.write_bytes(b"\x00" * 256)

    with settings_value(PORT_ENV, "ShowOutput", 0):
        # 1回目: 非表示で開始 → トグルで表示 → 終了時に 1 が保存される。
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert drv.find_output_bar() is not None, "アウトプットバーが見つからない"
            assert not drv.is_output_pane_visible(), "既定は非表示のはず"
            drv.toggle_output_pane()
            assert drv.is_output_pane_visible(), "トグルで表示になるはず"
        _wait_for_show_output(1)

        # 2回目: 表示状態で復元される → トグルで非表示 → 終了時に 0 が保存される。
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert drv.is_output_pane_visible(), "前回終了時の表示状態が復元されるはず"
            drv.toggle_output_pane()
            assert not drv.is_output_pane_visible(), "トグルで非表示になるはず"
        _wait_for_show_output(0)

        # 3回目: 非表示で復元される。
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            assert not drv.is_output_pane_visible(), "非表示状態が復元されるはず"


@pytest.mark.ported
def test_ported_env_settings_keeps_output_pane_state(ported_exe_path, tmp_path):
    """環境設定のOKで設定一式を保存しても、アウトプットペインの表示状態は巻き戻らない。

    環境設定はダイアログを開いた時点の設定の複製を確定させる。表示状態はダイアログに
    項目が無い実行時の値なので、表示切替の時点で設定へ取り込んでおかないと、OKの保存で
    古い値に巻き戻る。
    """
    test_file = tmp_path / "show_output_env.dat"
    test_file.write_bytes(b"\x00" * 256)

    with settings_value(PORT_ENV, "ShowOutput", 0):
        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.3)
            # ペインを表示してから環境設定を開き、そのままOKで閉じる。
            drv.toggle_output_pane()
            assert drv.is_output_pane_visible(), "トグルで表示になるはず"
            sheet = drv.open_env_settings_dialog()
            drv.close_settings_sheet(sheet, accept=True)
            time.sleep(0.5)
            assert drv.is_output_pane_visible(), "環境設定のOKで表示状態が変わらないはず"
            # OK の時点で設定一式が保存される。表示中の値が書かれていること。
            _wait_for_show_output(1)
