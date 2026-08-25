import time
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver


class TestGoldenCmdline:
    """Tests for command line argument handling in Stirling:
    - Paths with spaces
    - Multiple files specified on command line
    - Non-existent file error handling
    """

    @pytest.mark.original
    def test_original_cmdline_space_path(self, original_exe_path, tmp_path):
        """Verify Original Stirling opens a file whose path contains spaces."""
        space_dir = tmp_path / "stirling space directory"
        space_dir.mkdir(parents=True, exist_ok=True)
        test_file = space_dir / "space test file.dat"
        test_data = b"SPACE_PATH_ORIGINAL_TEST"
        test_file.write_bytes(test_data)

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            titles = drv.get_mdi_child_titles()
            assert any("space test file.dat" in t for t in titles), f"Expected space file in MDI titles: {titles}"

    @pytest.mark.ported
    def test_ported_cmdline_space_path(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling opens a file whose path contains spaces."""
        space_dir = tmp_path / "stirling space directory"
        space_dir.mkdir(parents=True, exist_ok=True)
        test_file = space_dir / "space test file port.dat"
        test_data = b"SPACE_PATH_PORTED_TEST"
        test_file.write_bytes(test_data)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            titles = drv.get_mdi_child_titles()
            assert any("space test file port.dat" in t for t in titles), f"Expected space file in MDI titles: {titles}"

    @pytest.mark.original
    def test_original_cmdline_multi_files(self, original_exe_path, tmp_path):
        """Verify Original Stirling opens all specified files in multiple MDI child windows."""
        file1 = tmp_path / "multi_file_1.dat"
        file2 = tmp_path / "multi_file_2.dat"
        file1.write_bytes(b"DATA_FILE_1")
        file2.write_bytes(b"DATA_FILE_2")

        with StirlingDriver(original_exe_path) as drv:
            drv.start(file1, file2)
            titles = drv.get_mdi_child_titles()
            assert len(titles) == 2, f"Expected 2 MDI child windows, got {len(titles)}: {titles}"
            assert any("multi_file_1.dat" in t for t in titles)
            assert any("multi_file_2.dat" in t for t in titles)

    @pytest.mark.ported
    def test_ported_cmdline_multi_files(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling opens all specified files in multiple MDI child windows."""
        file1 = tmp_path / "p_multi_file_1.dat"
        file2 = tmp_path / "p_multi_file_2.dat"
        file1.write_bytes(b"DATA_PORT_1")
        file2.write_bytes(b"DATA_PORT_2")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(file1, file2)
            titles = drv.get_mdi_child_titles()
            assert len(titles) == 2, f"Expected 2 MDI child windows, got {len(titles)}: {titles}"
            assert any("p_multi_file_1.dat" in t for t in titles)
            assert any("p_multi_file_2.dat" in t for t in titles)

    @pytest.mark.original
    def test_original_cmdline_non_existent_file(self, original_exe_path, tmp_path):
        """Verify Original Stirling shows an error dialog when a non-existent file path is passed on command line."""
        non_exist = tmp_path / "non_existent_file.dat"

        with StirlingDriver(original_exe_path) as drv:
            drv.start(non_exist)
            # Find the error dialog (#32770)
            h, title, items = drv.find_message_box(timeout=3.0)
            assert "Stirling" in title
            # Message should indicate file was not found
            assert any("non_existent_file.dat" in item or "見つかりません" in item for item in items)
            # Cleanly dismiss the dialog
            drv.dismiss_message_box()

    @pytest.mark.ported
    def test_ported_cmdline_non_existent_file(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling shows an error dialog when a non-existent file path is passed on command line."""
        non_exist = tmp_path / "p_non_existent_file.dat"

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(non_exist)
            h, title, items = drv.find_message_box(timeout=3.0)
            assert "StirHex" in title
            assert any("p_non_existent_file.dat" in item or "見つかりません" in item for item in items)
            drv.dismiss_message_box()
