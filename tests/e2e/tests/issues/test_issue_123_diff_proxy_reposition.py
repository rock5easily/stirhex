"""Issue #123: keep the diff list's minimized proxy reachable after the MDI
client shrinks.

The proxy is a plain WS_CHILD of MDICLIENT created at the bottom of the client
area.  Shrinking the main frame used to leave it at its original offset, so it
slipped completely outside the client area and the hidden dialog could no
longer be restored or closed.
"""

import time

import pytest
import win32con
import win32gui

from drivers.stirling_driver import StirlingDriver


def _wait_for(predicate, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError("condition did not become true")


def _open_diff_list(drv: StirlingDriver, first, second) -> int:
    drv.start(first)
    drv.open_file_via_dialog(second)
    compare = drv.open_compare_dialog()
    assert len(drv.compare_candidates(compare)) == 1
    drv.accept_compare(compare)
    return drv.find_diff_list_dialog()


def _mdi_client_screen_rect(drv: StirlingDriver) -> tuple[int, int, int, int]:
    mdi = drv.get_mdi_client()
    assert mdi, "MDI client not found"
    return win32gui.GetWindowRect(mdi)


class TestIssue123DiffProxyReposition:
    @pytest.mark.ported
    def test_proxy_stays_inside_mdi_after_frame_shrink(
        self, ported_exe_path, tmp_path
    ):
        first = tmp_path / "issue123_first.dat"
        second = tmp_path / "issue123_second.dat"
        first.write_bytes(bytes([0x00, 0x11, 0x22, 0x33]))
        second.write_bytes(bytes([0x00, 0xAA, 0x22, 0xBB]))

        with StirlingDriver(ported_exe_path) as drv:
            diff = _open_diff_list(drv, first, second)

            # Make sure the frame is in the restored state so it can be resized
            # (SW_RESTORE is a no-op for an already normal window).
            win32gui.ShowWindow(drv.hwnd, win32con.SW_RESTORE)
            time.sleep(0.2)
            main_left, main_top, main_right, main_bottom = win32gui.GetWindowRect(drv.hwnd)
            width = main_right - main_left
            height = main_bottom - main_top

            proxy = drv.minimize_diff_list(diff)
            assert not win32gui.IsWindowVisible(diff)
            assert win32gui.IsIconic(proxy)

            # The proxy sits at the bottom of the MDI client, so shrinking the
            # frame vertically is what used to push it out of view.
            shrunk_width = max(420, width - 260)
            shrunk_height = max(320, height - 300)
            win32gui.SetWindowPos(
                drv.hwnd, 0, main_left, main_top, shrunk_width, shrunk_height,
                win32con.SWP_NOZORDER | win32con.SWP_NOACTIVATE,
            )

            def _inside() -> bool:
                if not win32gui.IsWindow(proxy):
                    return False
                mdi_rect = _mdi_client_screen_rect(drv)
                p_left, p_top, p_right, p_bottom = win32gui.GetWindowRect(proxy)
                # The caption must stay reachable: the top-left corner inside
                # the client area and the window not pushed past its edges.
                return (mdi_rect[0] <= p_left and mdi_rect[1] <= p_top
                        and p_right <= mdi_rect[2] and p_bottom <= mdi_rect[3])

            _wait_for(_inside)

            # Both proxy operations must still work after the shrink.
            drv.restore_diff_list(diff)
            assert win32gui.IsWindowVisible(diff)
            assert not win32gui.IsWindow(proxy)

            proxy2 = drv.minimize_diff_list(diff)
            _wait_for(lambda: win32gui.IsWindow(proxy2))
            win32gui.SendMessage(proxy2, win32con.WM_SYSCOMMAND, win32con.SC_CLOSE, 0)
            _wait_for(lambda: not win32gui.IsWindow(diff))
            assert not win32gui.IsWindow(proxy2)
