import time
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver
from drivers.settings_context import stirling_settings


class TestIssue03StatusBar:
    """Tests for Issue #3: Status bar detailed data panes (Byte/Word/DWord/Float/Double) and Visibility setting.
    
    Prerequisite settings:
    - '環境設定' - 'ウィンドウ' -> 'ステータスバーの表示':
      - Enabled (show_status_bar = True): Status bar is visible with 7 indicator parts.
      - Disabled (show_status_bar = False): Status bar is hidden.
    """

    @pytest.mark.original
    def test_original_statusbar_shown(self, original_exe_path, tmp_path):
        """Verify Original Stirling status bar is visible and contains 7 indicator panes when enabled."""
        test_file = tmp_path / "statusbar_test_orig.dat"
        test_file.write_bytes(bytes([0x12, 0x34, 0x56, 0x78] * 4))

        with stirling_settings(show_status_bar=True):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                time.sleep(0.5)

                sb_info = drv.get_statusbar_info()
                assert sb_info is not None, "msctls_statusbar32 window not found"
                sb_hwnd, is_visible, part_count = sb_info
                assert is_visible, "Status bar should be visible"
                assert part_count == 7, f"Expected 7 status bar panes, got {part_count}"

    @pytest.mark.original
    def test_original_statusbar_hidden(self, original_exe_path, tmp_path):
        """Verify Original Stirling status bar is hidden when disabled."""
        test_file = tmp_path / "statusbar_hide_orig.dat"
        test_file.write_bytes(bytes([0x12, 0x34, 0x56, 0x78] * 4))

        with stirling_settings(show_status_bar=False):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                time.sleep(0.5)

                sb_info = drv.get_statusbar_info()
                if sb_info is not None:
                    _, is_visible, _ = sb_info
                    assert not is_visible, "Status bar should be hidden when show_status_bar is False"

    @pytest.mark.ported
    def test_ported_statusbar_shown(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling status bar is visible and contains 7 indicator panes when enabled."""
        test_file = tmp_path / "statusbar_test_port.dat"
        test_file.write_bytes(bytes([0x12, 0x34, 0x56, 0x78] * 4))

        with stirling_settings(show_status_bar=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                time.sleep(0.5)

                sb_info = drv.get_statusbar_info()
                assert sb_info is not None, "msctls_statusbar32 window not found"
                sb_hwnd, is_visible, part_count = sb_info
                assert is_visible, "Status bar should be visible"
                assert part_count == 7, f"Expected 7 status bar panes, got {part_count}"

    @pytest.mark.ported
    def test_ported_statusbar_hidden(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling status bar is hidden when disabled."""
        test_file = tmp_path / "statusbar_hide_port.dat"
        test_file.write_bytes(bytes([0x12, 0x34, 0x56, 0x78] * 4))

        with stirling_settings(show_status_bar=False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                time.sleep(0.5)

                sb_info = drv.get_statusbar_info()
                if sb_info is not None:
                    _, is_visible, _ = sb_info
                    assert not is_visible, "Status bar should be hidden when show_status_bar is False"
