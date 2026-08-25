import time
import pytest
from pathlib import Path
import win32gui
import win32con
from pywinauto import timings
from drivers.stirling_driver import StirlingDriver, safe_set_focus
from drivers.settings_context import stirling_settings


class TestIssue04FileWatch:
    """Tests for Issue #4: External file modification detection and notification dialog.
    
    Prerequisite settings:
    - '環境設定' - 'ファイル': 'ファイルの排他制御' must be set to 'しない' (file_exclusive_mode = 0).
    """

    @pytest.mark.original
    def test_original_external_file_modification_detection(self, original_exe_path, tmp_path):
        """Verify Original Stirling prompts user when file is externally changed (with exclusive mode = None)."""
        test_file = tmp_path / "watch_test.dat"
        test_file.write_bytes(b"INITIAL_ORIGINAL_FILE_CONTENT_12345")

        # Setup: Ensure prerequisite setting (File exclusive mode = 0 "しない") and Teardown afterwards
        with stirling_settings(file_exclusive_mode=0):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                time.sleep(0.5)

                # Modify file on disk externally (wait > 1s for filesystem timestamp granularity)
                time.sleep(1.2)
                test_file.write_bytes(b"EXTERNALLY_MODIFIED_FILE_CONTENT_9999")

                # Activate another window and reactivate Stirling view
                tray = win32gui.FindWindow("Shell_TrayWnd", None)
                if tray:
                    # 目的は「いったん別ウィンドウへフォーカスを移す」ことだけ。
                    # 前面化の権利がない状況では SetForegroundWindow が失敗するが、
                    # 直後の safe_set_focus で Stirling を掴み直すので致命ではない。
                    try:
                        win32gui.SetForegroundWindow(tray)
                    except Exception:
                        pass
                time.sleep(0.5)

                safe_set_focus(drv.hwnd)
                view_hwnd = drv.get_view_hwnd()
                if view_hwnd:
                    win32gui.PostMessage(view_hwnd, 0x0400 + 0x1B, 0, 0)
                time.sleep(1.0)

                # Check if external modification dialog (#32770) appears
                def _find_watch_dialog():
                    wins = drv._get_process_windows()
                    for h, cls, title in wins:
                        if cls == "#32770":
                            return h
                    raise RuntimeError("External modification dialog not shown yet")

                dlg_hwnd = timings.wait_until_passes(5, 0.5, _find_watch_dialog)
                assert dlg_hwnd != 0
                # Dismiss dialog (Cancel / ESC)
                win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, win32con.IDCANCEL, 0)
                time.sleep(0.5)

    @pytest.mark.ported
    def test_ported_external_file_modification_detection_and_reload(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling detects external file changes and reloads content on IDOK (default option 1017: reload)."""
        test_file = tmp_path / "ported_watch_test.dat"
        test_file.write_bytes(b"INITIAL_PORTED_FILE_CONTENT_12345")

        # Setup: Ensure prerequisite setting and Teardown afterwards
        with stirling_settings(file_exclusive_mode=0):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                time.sleep(0.5)

                # Modify file on disk externally
                time.sleep(1.2)
                new_data = b"EXTERNALLY_MODIFIED_PORTED_CONTENT_9999"
                test_file.write_bytes(new_data)

                # Switch focus to shell tray then return focus to Stirling view
                tray = win32gui.FindWindow("Shell_TrayWnd", None)
                if tray:
                    # 目的は「いったん別ウィンドウへフォーカスを移す」ことだけ。
                    # 前面化の権利がない状況では SetForegroundWindow が失敗するが、
                    # 直後の safe_set_focus で Stirling を掴み直すので致命ではない。
                    try:
                        win32gui.SetForegroundWindow(tray)
                    except Exception:
                        pass
                time.sleep(0.5)

                safe_set_focus(drv.hwnd)
                view_hwnd = drv.get_view_hwnd()
                if view_hwnd:
                    win32gui.PostMessage(view_hwnd, 0x0400 + 0x1B, 0, 0)
                time.sleep(1.0)

                def _find_watch_dialog():
                    wins = drv._get_process_windows()
                    for h, cls, title in wins:
                        if cls == "#32770":
                            return h
                    raise RuntimeError("External modification dialog not shown in ported build")

                dlg_hwnd = timings.wait_until_passes(5, 0.5, _find_watch_dialog)
                assert dlg_hwnd != 0

                # Default selection is 1017 (Reload). Click OK (IDOK = 1)
                win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)
                time.sleep(0.5)

                # Save to verify reloaded content
                save_out = tmp_path / "saved_after_reload.dat"
                drv.save_as_via_dialog(save_out)
                assert save_out.read_bytes() == new_data, "File content in editor should match updated disk content after reload"

    @pytest.mark.ported
    def test_ported_external_file_modification_ignore_persists(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling ignores changes and does not prompt again for the same change when '無視' (1016) is chosen."""
        test_file = tmp_path / "ported_watch_ignore.dat"
        test_file.write_bytes(b"INITIAL_PORTED_FILE_CONTENT_ABCDEF")

        with stirling_settings(file_exclusive_mode=0):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                time.sleep(0.5)

                time.sleep(1.2)
                test_file.write_bytes(b"EXTERNALLY_MODIFIED_PORTED_IGNORE_XYZ")

                safe_set_focus(drv.hwnd)
                view_hwnd = drv.get_view_hwnd()
                if view_hwnd:
                    win32gui.PostMessage(view_hwnd, 0x0400 + 0x1B, 0, 0)
                time.sleep(1.0)

                def _find_watch_dialog():
                    wins = drv._get_process_windows()
                    for h, cls, title in wins:
                        if cls == "#32770":
                            return h
                    raise RuntimeError("External modification dialog not shown")

                dlg_hwnd = timings.wait_until_passes(5, 0.5, _find_watch_dialog)
                
                # Select option 1016 (Ignore)
                btn_ignore = win32gui.GetDlgItem(dlg_hwnd, 1016)
                if btn_ignore:
                    win32gui.SendMessage(btn_ignore, win32con.BM_CLICK, 0, 0)
                    time.sleep(0.1)

                # Click OK
                win32gui.PostMessage(dlg_hwnd, win32con.WM_COMMAND, 1, 0)
                time.sleep(0.5)

                # Reactivate view again - dialog should NOT appear again for the same modification
                win32gui.PostMessage(view_hwnd, 0x0400 + 0x1B, 0, 0)
                time.sleep(0.5)

                # Verify no dialog popped up
                dialogs = [h for h, cls, _ in drv._get_process_windows() if cls == "#32770"]
                assert len(dialogs) == 0, f"Expected no dialog after ignore, got {dialogs}"
