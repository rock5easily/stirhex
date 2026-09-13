import time
import pytest
from pathlib import Path
import win32gui
import win32con
from pywinauto import timings
from drivers.stirling_driver import StirlingDriver, safe_set_focus
from drivers.settings_context import stirling_settings

# Fixed content for the Save As / compare / cancel cases (Issue #205).
#   The editor overwrites the first byte with 0x11, so the three versions - on disk,
#   in the editor, and after a second external change - stay distinguishable.
ORIGINAL_CONTENT = b"WATCH_ORIGINAL_CONTENT_0123456789"
EDITED_CONTENT = b"" + ORIGINAL_CONTENT[1:]
EXTERNAL_CONTENT = b"WATCH_EXTERNAL_CONTENT_ABCDEFGHIJ"
SECOND_EXTERNAL_CONTENT = b"WATCH_EXTERNAL_AGAIN_QRSTUVWXYZ12"


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

    # --- Save As / compare / cancel from the notification (Issue #205) ---
    #
    # The cases above cover reloading and ignoring. CStirlingView::OnCheckFileChanged has
    # three more outcomes that nothing exercised: saving the edited data elsewhere (with
    # the document following the new path, optionally opening the changed file to compare
    # it), and closing the dialog without deciding, which must ask again.

    @pytest.mark.ported
    def test_ported_external_change_save_as_keeps_both_versions(self, ported_exe_path, tmp_path):
        """Save As from the dialog writes the edit; the changed file keeps its new content."""
        test_file = tmp_path / "watch_saveas.dat"
        test_file.write_bytes(ORIGINAL_CONTENT)
        destination = tmp_path / "watch_saveas_copy.dat"

        with stirling_settings(file_exclusive_mode=0):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.4)

                # Unsaved edit in the editor ...
                drv.type_hex_chars("11")
                time.sleep(0.3)
                # ... and a different change on disk.
                time.sleep(1.2)
                test_file.write_bytes(EXTERNAL_CONTENT)

                drv.notify_file_changed()
                drv.answer_file_changed_dialog("save_as", save_as=destination)
                time.sleep(0.5)

                assert destination.read_bytes() == EDITED_CONTENT, (
                    "the destination must hold what was being edited, not the disk content"
                )
                assert test_file.read_bytes() == EXTERNAL_CONTENT, (
                    "the externally changed file must be left as it is"
                )
                assert any(
                    destination.name in title for title in drv.get_mdi_child_titles()
                ), f"the document must follow the new path: {drv.get_mdi_child_titles()}"

    @pytest.mark.ported
    def test_ported_external_change_save_as_and_compare(self, ported_exe_path, tmp_path):
        """Ticking "compare" opens the changed file and runs a comparison against it."""
        test_file = tmp_path / "watch_compare.dat"
        test_file.write_bytes(ORIGINAL_CONTENT)
        destination = tmp_path / "watch_compare_copy.dat"

        with stirling_settings(file_exclusive_mode=0):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.4)

                drv.type_hex_chars("11")
                time.sleep(0.3)
                time.sleep(1.2)
                test_file.write_bytes(EXTERNAL_CONTENT)

                drv.notify_file_changed()
                drv.answer_file_changed_dialog("save_as", save_as=destination, compare=True)
                time.sleep(1.0)

                titles = drv.get_mdi_child_titles()
                assert any(destination.name in title for title in titles), (
                    f"the edited document must be saved under the new name: {titles}"
                )
                assert any(test_file.name in title for title in titles), (
                    f"the changed file must be opened for the comparison: {titles}"
                )

    @pytest.mark.ported
    def test_ported_external_change_cancel_asks_again(self, ported_exe_path, tmp_path):
        """Closing the dialog without deciding keeps the edit and asks again."""
        test_file = tmp_path / "watch_cancel.dat"
        test_file.write_bytes(ORIGINAL_CONTENT)

        with stirling_settings(file_exclusive_mode=0):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.4)

                drv.type_hex_chars("11")
                time.sleep(0.3)
                time.sleep(1.2)
                test_file.write_bytes(EXTERNAL_CONTENT)

                drv.notify_file_changed()
                drv.answer_file_changed_dialog("cancel")

                # Nothing was decided, so the next activation must ask once more.
                drv.notify_file_changed()
                assert drv.find_file_changed_dialog(timeout=5.0), (
                    "cancelling must leave the question open"
                )
                drv.answer_file_changed_dialog("cancel")

                # The edit is still in the editor: saving it elsewhere proves it.
                kept = tmp_path / "watch_cancel_kept.dat"
                drv.save_as_via_dialog(kept)
                assert kept.read_bytes() == EDITED_CONTENT, (
                    "cancelling must not touch what is being edited"
                )

    @pytest.mark.ported
    def test_ported_external_change_ignore_then_change_again(self, ported_exe_path, tmp_path):
        """After ignoring, a further external change is reported again."""
        test_file = tmp_path / "watch_ignore_again.dat"
        test_file.write_bytes(ORIGINAL_CONTENT)

        with stirling_settings(file_exclusive_mode=0):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.4)

                drv.type_hex_chars("11")
                time.sleep(0.3)
                time.sleep(1.2)
                test_file.write_bytes(EXTERNAL_CONTENT)

                drv.notify_file_changed()
                drv.answer_file_changed_dialog("ignore")

                # A second, different change on disk is a new event and must be reported.
                time.sleep(1.2)
                test_file.write_bytes(SECOND_EXTERNAL_CONTENT)
                drv.notify_file_changed()
                assert drv.find_file_changed_dialog(timeout=5.0), (
                    "a later external change must be reported even after ignoring one"
                )
                drv.answer_file_changed_dialog("ignore")

                # The edit survived both notifications.
                kept = tmp_path / "watch_ignore_kept.dat"
                drv.save_as_via_dialog(kept)
                assert kept.read_bytes() == EDITED_CONTENT, (
                    "ignoring must keep the edited data in memory"
                )
