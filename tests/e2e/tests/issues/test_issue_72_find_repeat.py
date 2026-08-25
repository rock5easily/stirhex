import time

import pytest
import win32con
import win32gui

from drivers.stirling_driver import StirlingDriver

# [検索・移動] の繰り返し検索。原版と同じコマンド ID。
ID_FIND_NEXT = 32823   # 0x8037
ID_FIND_PREV = 32822   # 0x8036
ID_EDIT_FIND = 57636   # ID_EDIT_FIND（MFC 標準）


def _dialog_titles(drv: StirlingDriver) -> list[str]:
    return [title for _h, cls, title in drv._get_process_windows() if cls == "#32770"]


def _wait_for_dialog(drv: StirlingDriver, timeout: float = 4.0) -> list[str]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        titles = _dialog_titles(drv)
        if titles:
            return titles
        time.sleep(0.2)
    return []


class TestIssue72FindRepeat:
    """Issue #72: 直近の検索条件が無い状態の [次検索]／[前検索]。

    原版 CStirlingView_FindNextImpl(0x44b486) は view+0x1d0（前回パターン長）が 0 のとき
    CSearchDlg を DoModal する。原版のメッセージマップには 0x8036/0x8037 の
    ON_UPDATE_COMMAND_UI が無く、コマンドは常に実行できる。

    移植版は一時期この 2 コマンドへ「直近検索がある時のみ活性」の更新ハンドラを付けていた。
    MFC は無効なコマンドの WM_COMMAND を配送しないため、キー割り当てから実行しても
    ハンドラに届かず、何も起きなかった。
    """

    @pytest.mark.ported
    @pytest.mark.parametrize("command_id", [ID_FIND_NEXT, ID_FIND_PREV])
    def test_repeat_without_condition_opens_find_dialog(self, ported_exe_path, tmp_path, command_id):
        """検索条件が無い状態で繰り返し検索を実行すると、検索ダイアログが開く。"""
        test_file = tmp_path / "find_repeat.dat"
        test_file.write_bytes(bytes(range(32)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)
            assert _dialog_titles(drv) == [], "起動直後にダイアログが出ている"

            view = drv.get_view_hwnd()
            assert view, "ビューのウィンドウが見つからない"
            # モーダルダイアログが開くため、SendMessage ではなく PostMessage を使う。
            win32gui.PostMessage(view, win32con.WM_COMMAND, command_id, 0)

            titles = _wait_for_dialog(drv)
            assert titles, f"コマンド {command_id} で検索ダイアログが開かない（Issue #72 の再発）"
            assert "検索" in titles[0], f"想定外のダイアログ: {titles}"

            # 開いたダイアログを閉じてから終了する（後片付け）。
            for h, cls, _title in drv._get_process_windows():
                if cls == "#32770":
                    win32gui.PostMessage(h, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
                    break
            time.sleep(0.3)

    @pytest.mark.ported
    @pytest.mark.parametrize("vk, shift", [(win32con.VK_F3, False), (win32con.VK_F3, True)])
    def test_default_key_opens_find_dialog(self, ported_exe_path, tmp_path, vk, shift):
        """既定キー（F3 / Shift+F3）でも検索ダイアログが開く。Issue #72 の症状そのもの。"""
        import ctypes

        test_file = tmp_path / "find_repeat_key.dat"
        test_file.write_bytes(bytes(range(32)))

        user32 = ctypes.windll.user32
        KEYEVENTF_KEYUP = 0x0002

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)
            view = drv.get_view_hwnd()
            assert view, "ビューのウィンドウが見つからない"

            # KeymapLookup は GetAsyncKeyState で修飾キーを見るため、Shift は実キーで押さえる。
            if shift:
                user32.keybd_event(win32con.VK_SHIFT, 0, 0, 0)
                time.sleep(0.05)
            try:
                # ダイアログはモーダルなので PostMessage で送る（SendMessage だとブロックする）。
                win32gui.PostMessage(view, win32con.WM_KEYDOWN, vk, 0)
                win32gui.PostMessage(view, win32con.WM_KEYUP, vk, 0)
                titles = _wait_for_dialog(drv)
            finally:
                if shift:
                    user32.keybd_event(win32con.VK_SHIFT, 0, KEYEVENTF_KEYUP, 0)

            assert titles, "既定キーで検索ダイアログが開かない（Issue #72 の再発）"
            assert "検索" in titles[0], f"想定外のダイアログ: {titles}"

            for h, cls, _title in drv._get_process_windows():
                if cls == "#32770":
                    win32gui.PostMessage(h, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
                    break
            time.sleep(0.3)

    @pytest.mark.ported
    def test_repeat_command_is_enabled_in_menu(self, ported_exe_path, tmp_path):
        """メニューの [次検索]／[前検索] は、検索条件が無くても選択できる（原版と同じ）。"""
        test_file = tmp_path / "find_repeat_menu.dat"
        test_file.write_bytes(bytes(range(32)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            time.sleep(0.5)
            menu = win32gui.GetMenu(drv.hwnd)
            assert menu, "メニューが取得できない"
            # WM_INITMENUPOPUP を経ないと更新ハンドラが走らないため、[検索・移動] を明示的に更新する。
            search_menu = win32gui.GetSubMenu(menu, 2)
            win32gui.SendMessage(drv.hwnd, win32con.WM_INITMENUPOPUP,
                                 search_menu, win32api_makelparam(2, 0))
            time.sleep(0.2)
            for command_id in (ID_FIND_NEXT, ID_FIND_PREV):
                state = win32gui.GetMenuState(search_menu, command_id, win32con.MF_BYCOMMAND)
                assert state != -1, f"コマンド {command_id} がメニューに無い"
                assert not (state & win32con.MF_GRAYED), \
                    f"コマンド {command_id} が無効になっている（Issue #72 の再発）"


def win32api_makelparam(low: int, high: int) -> int:
    return (high << 16) | (low & 0xFFFF)
