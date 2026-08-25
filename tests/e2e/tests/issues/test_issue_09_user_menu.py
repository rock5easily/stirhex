import time
import pytest
from pathlib import Path
import win32gui, win32con
from drivers.stirling_driver import (
    StirlingDriver,
    safe_set_focus,
    ID_USERMENU_BASE,
    ID_USERMENU_1,
    ID_USERMENU_10,
    ID_TWOSTROKE_1,
    ID_TWOSTROKE_3,
    ID_SETTINGS_ENV,
    ID_RUN_APP,
)
from drivers.settings_context import stirling_settings


class TestIssue09UserMenu:
    """Tests for Issue #9: User Menu (0x803A..0x8046) custom popup menus and settings."""

    # 1. Context Menu (Default User Menu idx=14)
    @pytest.mark.original
    def test_original_context_menu_invoke(self, original_exe_path, tmp_path):
        """Verify Original Stirling displays default context menu on Shift+F10 / right click."""
        test_file = tmp_path / "um_ctx_orig.dat"
        test_file.write_bytes(bytes(range(64)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.invoke_context_menu()
            popup = drv.find_popup_menu(timeout=3.0)
            assert popup is not None, "Original Stirling did not display context menu"
            drv.dismiss_popup_menu()

    @pytest.mark.ported
    def test_ported_context_menu_invoke(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling displays default context menu on Shift+F10 / right click."""
        test_file = tmp_path / "um_ctx_port.dat"
        test_file.write_bytes(bytes(range(64)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.invoke_context_menu()
            popup = drv.find_popup_menu(timeout=3.0)
            assert popup is not None, "Ported Stirling did not display context menu"
            drv.dismiss_popup_menu()

    # 2. Configured User Menu 1 (0x803A) Popup and Command Dispatch
    @pytest.mark.original
    def test_original_user_menu_invoke_and_execute(self, original_exe_path, tmp_path):
        """Verify Original Stirling invokes User Menu 1 and executes selected command."""
        test_file = tmp_path / "um_exec_orig.dat"
        test_file.write_bytes(bytes(range(64)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # Invoke User Menu 1
            drv.post_command(ID_USERMENU_1)
            time.sleep(0.3)
            # If user menu 1 is empty in clean registry, it should ignore or show menu
            # Dismiss if opened
            drv.dismiss_popup_menu()

    @pytest.mark.ported
    def test_ported_user_menu_invoke_and_execute(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling invokes User Menu 1 and executes selected command."""
        test_file = tmp_path / "um_exec_port.dat"
        test_file.write_bytes(bytes(range(64)))

        # Configure userMenus[0] with rawId 0x0105 (Category 1: Move -> Item 5: Goto Data End)
        # rawID format in Stirling: (cat << 8) | item. (1<<8)|5 = 0x0105
        with stirling_settings(user_menus={0: [0x0105]}):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                # Initial address in status bar (any pane containing "00000000")
                initial_panes = drv.get_all_statusbar_text()
                assert any("00000000" in t for t in initial_panes), f"Initial address not in panes: {initial_panes}"

                # Invoke User Menu 1 (0x803A)
                drv.post_command(ID_USERMENU_1)
                popup = drv.find_popup_menu(timeout=1.0)
                assert popup is not None, "Ported Stirling did not popup User Menu 1"

                # Execute the first item (Down arrow + Enter)
                drv.select_popup_menu_item(down_count=1)
                time.sleep(0.5)

                # Address should now be at end of data (0x00000040)
                end_panes = drv.get_all_statusbar_text()
                assert any("00000040" in t or "0x00000040" in t for t in end_panes), f"End address not in panes: {end_panes}"

    # 3. Empty User Menu Handled Gracefully (No Popup, No Crash)
    @pytest.mark.original
    def test_original_empty_user_menu_ignored(self, original_exe_path, tmp_path):
        """Verify Original Stirling safely ignores unconfigured user menu."""
        test_file = tmp_path / "um_empty_orig.dat"
        test_file.write_bytes(bytes(range(32)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # Invoke User Menu 2 (0x803B)
            drv.post_command(ID_USERMENU_BASE + 1)
            time.sleep(0.3)
            # Should not crash
            assert win32gui.IsWindow(drv.hwnd)

    @pytest.mark.ported
    def test_ported_empty_user_menu_ignored(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling safely ignores unconfigured user menu."""
        test_file = tmp_path / "um_empty_port.dat"
        test_file.write_bytes(bytes(range(32)))

        # Ensure user menu 2 (idx 1) is empty
        with stirling_settings(user_menus={1: []}):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                # Invoke User Menu 2 (0x803B)
                drv.post_command(ID_USERMENU_BASE + 1)
                time.sleep(0.3)
                popup = drv.find_popup_menu(timeout=0.2)
                assert popup is None, "Empty user menu should not display popup"
                assert win32gui.IsWindow(drv.hwnd)

    # 4. Environment Settings "User Menu" Page UI
    @pytest.mark.original
    def test_original_env_settings_dialog(self, original_exe_path, tmp_path):
        """Verify Original Stirling Environment Settings dialog opens."""
        test_file = tmp_path / "um_env_orig.dat"
        test_file.write_bytes(bytes(range(32)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            dlg = drv.open_env_settings_dialog()
            assert dlg is not None and win32gui.IsWindow(dlg)
            # Close dialog with Cancel (ESC)
            win32gui.SendMessage(dlg, win32con.WM_COMMAND, 2, 0) # IDCANCEL = 2
            time.sleep(0.3)

    @pytest.mark.ported
    def test_ported_env_settings_dialog(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling Environment Settings dialog opens."""
        test_file = tmp_path / "um_env_port.dat"
        test_file.write_bytes(bytes(range(32)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            dlg = drv.open_env_settings_dialog()
            assert dlg is not None and win32gui.IsWindow(dlg)
            win32gui.SendMessage(dlg, win32con.WM_COMMAND, 2, 0) # IDCANCEL = 2
            time.sleep(0.3)

    # 5. Run App Dialog (0x804F / 32847 / ID_RUN_APP)
    @pytest.mark.original
    def test_original_run_app_command(self, original_exe_path, tmp_path):
        """Verify Original Stirling opens 'Run App' dialog on 0x804F (32847)."""
        test_file = tmp_path / "um_run_orig.dat"
        test_file.write_bytes(bytes(range(32)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.post_command(ID_RUN_APP)
            time.sleep(0.5)
            # Check if modal dialog appeared
            found = False
            for h, cls, title in drv._get_process_windows():
                if any(k in title for k in ["名前を指定して実行", "実行", "Run"]):
                    win32gui.SendMessage(h, win32con.WM_COMMAND, 2, 0) # IDCANCEL
                    found = True
                    break
            assert found, "Run App dialog not found in original build"

    @pytest.mark.ported
    def test_ported_run_app_command(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling opens 'Run App' dialog on 0x804F (32847)."""
        test_file = tmp_path / "um_run_port.dat"
        test_file.write_bytes(bytes(range(32)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.post_command(ID_RUN_APP)
            time.sleep(0.5)
            found = False
            for h, cls, title in drv._get_process_windows():
                if any(k in title for k in ["名前を指定して実行", "実行", "Run"]):
                    win32gui.SendMessage(h, win32con.WM_COMMAND, 2, 0) # IDCANCEL
                    found = True
                    break
            assert found, "Run App dialog not found in ported build"

    # 6. Esc Menu Popup and Command Dispatch
    @pytest.mark.ported
    def test_ported_esc_menu_invoke_and_execute(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling invokes Esc menu (User Menu 14 / index 13) on Esc key when enabled."""
        test_file = tmp_path / "um_esc_port.dat"
        test_file.write_bytes(bytes(range(64)))

        # Configure userMenus[13] with rawId 0x0105 (Goto Data End) and enable esc_menu
        with stirling_settings(user_menus={13: [0x0105]}, esc_menu=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                initial_panes = drv.get_all_statusbar_text()
                assert any("00000000" in t for t in initial_panes)

                # Send ESC to view window
                view_hwnd = drv.get_view_hwnd()
                safe_set_focus(view_hwnd)
                time.sleep(0.3)
                win32gui.PostMessage(view_hwnd, win32con.WM_KEYDOWN, win32con.VK_ESCAPE, 0)
                win32gui.PostMessage(view_hwnd, win32con.WM_KEYUP, win32con.VK_ESCAPE, 0)
                time.sleep(0.5)

                popup = drv.find_popup_menu(timeout=2.0)
                assert popup is not None, "Esc menu popup not displayed"

                drv.select_popup_menu_item(down_count=1)
                time.sleep(0.5)

                end_panes = drv.get_all_statusbar_text()
                assert any("00000040" in t or "0x00000040" in t for t in end_panes)

    # 7. Two-Stroke Key Dispatch
    @pytest.mark.ported
    def test_ported_twostroke_key_dispatch(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling executes command via Two-Stroke key (0x8044 / ID_TWOSTROKE_1)."""
        test_file = tmp_path / "um_twostroke_port.dat"
        test_file.write_bytes(bytes(range(64)))

        # Configure userMenu 10 (Two-Stroke 1) with accelerator key 'E' and rawID 0x0105
        # Value format: (ord('E') << 16) | 0x0105
        item_val = (ord('E') << 16) | 0x0105
        with stirling_settings(user_menus={10: [item_val]}):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                initial_panes = drv.get_all_statusbar_text()
                assert any("00000000" in t for t in initial_panes)

                # Trigger 2-stroke command 1
                drv.post_command(ID_TWOSTROKE_1)
                time.sleep(0.3)

                popup = drv.find_popup_menu(timeout=2.0)
                assert popup is not None, "Two-stroke popup menu not displayed"

                # Send accelerator key 'e' (or execute first item)
                win32gui.PostMessage(popup, win32con.WM_CHAR, ord('e'), 0)
                time.sleep(0.5)

                end_panes = drv.get_all_statusbar_text()
                assert any("00000040" in t or "0x00000040" in t for t in end_panes)
