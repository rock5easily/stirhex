"""Issue #242: 設定値の読込時正規化と同期グループ再構築の回帰テスト。"""

import ctypes
import time
import winreg

import pytest
import win32con
import win32gui

from drivers.settings_context import read_reg_values, registry_section
from drivers.stirling_driver import IDC_SYNC_ADD, StirlingDriver


PORT_ENV = r"Software\StirHex\StirHex\Env"


@pytest.mark.ported
@pytest.mark.parametrize(
    "initial, expected",
    [
        pytest.param(
            {
                "ScrollLines": (5000, winreg.REG_DWORD),
                "TwoStrokeTimeoutMs": (2501, winreg.REG_DWORD),
                "UndoMemoryLimit": (1, winreg.REG_DWORD),
                "UndoMemoryLimitMB": (70000, winreg.REG_DWORD),
                "LargeFileWarnMB": (2000000, winreg.REG_DWORD),
            },
            {
                "ScrollLines": 999,
                "TwoStrokeTimeoutMs": 2000,
                "UndoMemoryLimit": 1,
                "UndoMemoryLimitMB": 65536,
                "LargeFileWarnMB": 1048576,
            },
            id="upper-bounds",
        ),
        pytest.param(
            {
                "ScrollLines": (0, winreg.REG_DWORD),
                "TwoStrokeTimeoutMs": (1555, winreg.REG_DWORD),
                "UndoMemoryLimit": (1, winreg.REG_DWORD),
                "UndoMemoryLimitMB": (0, winreg.REG_DWORD),
                "LargeFileWarnMB": (0, winreg.REG_DWORD),
            },
            {
                "ScrollLines": 1,
                "TwoStrokeTimeoutMs": 1600,
                "UndoMemoryLimit": 0,
                "UndoMemoryLimitMB": 256,
                "LargeFileWarnMB": 512,
            },
            id="lower-bounds-and-rounding",
        ),
    ],
)
def test_loaded_settings_are_normalized(ported_exe_path, initial, expected):
    """手編集された範囲外値を起動時にUIの有効範囲へ正規化する。"""
    with registry_section(PORT_ENV, initial):
        with StirlingDriver(ported_exe_path) as drv:
            drv.start()

        saved = read_reg_values(PORT_ENV)
        for name, value in expected.items():
            assert int(saved[name][0]) == value, (
                f"{name} was not normalized: {saved.get(name)!r}"
            )


@pytest.mark.ported
def test_replacing_group_detaches_new_members_from_their_old_group(
    ported_exe_path, tmp_path
):
    """A-BをC-Bへ置き換えた後、旧メンバーAからBへの片方向同期を残さない。"""
    paths = [tmp_path / f"sync_replace_{name}.dat" for name in "abc"]
    for index, path in enumerate(paths):
        path.write_bytes(bytes([index]) * 8192)

    with StirlingDriver(ported_exe_path) as drv:
        drv.start(paths[0])
        drv.open_file_via_dialog(paths[1])
        drv.open_file_via_dialog(paths[2])

        views = drv.get_mdi_views()
        by_name = {
            path.name: next(item for item in views if path.name in item[0])
            for path in paths
        }

        # 最初に A-B の同期グループを作る。
        drv.activate_mdi_child(by_name[paths[0].name][1])
        dialog = drv.open_sync_scroll_dialog()
        drv.sync_scroll_select_candidate(dialog, paths[1].name)
        drv.click_dialog_button(dialog, IDC_SYNC_ADD)
        drv.click_dialog_button(dialog, win32con.IDOK)

        # B を新規メンバーとして C-B に組み替える。
        drv.activate_mdi_child(by_name[paths[2].name][1])
        dialog = drv.open_sync_scroll_dialog()
        drv.sync_scroll_select_candidate(dialog, paths[1].name)
        drv.click_dialog_button(dialog, IDC_SYNC_ADD)
        drv.click_dialog_button(dialog, win32con.IDOK)

        view_a = by_name[paths[0].name][2]
        view_b = by_name[paths[1].name][2]
        view_c = by_name[paths[2].name][2]
        get_scroll_pos = ctypes.windll.user32.GetScrollPos

        # 旧メンバー A のスクロールは、BにもCにも伝わらない。
        for _ in range(2):
            win32gui.SendMessage(
                view_a, win32con.WM_VSCROLL, win32con.SB_PAGEDOWN, 0
            )
        time.sleep(0.3)
        pos_a = get_scroll_pos(view_a, win32con.SB_VERT)
        assert pos_a > 0
        assert get_scroll_pos(view_b, win32con.SB_VERT) == 0
        assert get_scroll_pos(view_c, win32con.SB_VERT) == 0

        # 新グループ C-B の同期は引き続き双方向に機能する。
        win32gui.SendMessage(
            view_c, win32con.WM_VSCROLL, win32con.SB_PAGEDOWN, 0
        )
        time.sleep(0.3)
        pos_c = get_scroll_pos(view_c, win32con.SB_VERT)
        assert pos_c > 0
        assert get_scroll_pos(view_b, win32con.SB_VERT) == pos_c
        assert get_scroll_pos(view_a, win32con.SB_VERT) == pos_a
