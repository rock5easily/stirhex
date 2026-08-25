import time
import subprocess
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver
from drivers.settings_context import stirling_settings


def create_windows_shortcut(target_path: Path, lnk_path: Path):
    """Create a Windows .lnk shortcut file pointing to target_path using PowerShell WScript.Shell."""
    target_str = str(target_path.resolve()).replace("'", "''")
    lnk_str = str(lnk_path.resolve()).replace("'", "''")
    ps_script = f"""
    $WshShell = New-Object -ComObject WScript.Shell
    $Shortcut = $WshShell.CreateShortcut('{lnk_str}')
    $Shortcut.TargetPath = '{target_str}'
    $Shortcut.Save()
    """
    subprocess.run(["powershell", "-NoProfile", "-Command", ps_script], check=True)


class TestIssue11LnkFile:
    """Tests for Issue #11: Handling .lnk Windows shortcuts depending on open route and settings.
    
    Prerequisite settings:
    - Route 1 (Command line / D&D / Send-To):
      - Always opens .lnk raw binary regardless of settings.
    - Route 2 ('File - Open' dialog):
      - '環境設定' - 'ファイル' -> 'リンクファイルは直接開く':
        - Unchecked (open_lnk_direct = False, Default): Resolves and opens the link target file.
        - Checked (open_lnk_direct = True): Opens the .lnk raw binary.
    """

    @pytest.mark.original
    def test_original_lnk_opened_via_commandline_opens_raw_binary(self, original_exe_path, tmp_path):
        """Verify Original Stirling opens .lnk raw binary when opened via command line argument or D&D."""
        target_file = tmp_path / "actual_payload_cmd.dat"
        lnk_file = tmp_path / "shortcut_cmd.lnk"
        out_file = tmp_path / "out_cmd_orig.dat"

        payload_content = b"PAYLOAD_VIA_COMMANDLINE_ARG_12345"
        target_file.write_bytes(payload_content)

        create_windows_shortcut(target_file, lnk_file)
        assert lnk_file.exists()

        # Open via command line argument
        with StirlingDriver(original_exe_path) as drv:
            drv.start(lnk_file)
            time.sleep(0.5)
            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        # Windows shortcut files start with header magic 0x4C ('L')
        saved_bytes = out_file.read_bytes()
        assert saved_bytes.startswith(b"L\x00\x00\x00"), "Original Stirling should open raw .lnk binary via command line"

    @pytest.mark.original
    def test_original_lnk_opened_via_open_dialog_resolves_target(self, original_exe_path, tmp_path):
        """Verify Original Stirling resolves link target when opened via 'File - Open' dialog with default setting."""
        target_file = tmp_path / "actual_payload_dlg.dat"
        lnk_file = tmp_path / "shortcut_dlg.lnk"
        out_file = tmp_path / "out_dlg_orig.dat"

        payload_content = b"PAYLOAD_VIA_OPEN_DIALOG_TARGET_RESOLVED_67890"
        target_file.write_bytes(payload_content)

        create_windows_shortcut(target_file, lnk_file)
        assert lnk_file.exists()

        # Setup: Ensure prerequisite setting (open_lnk_direct = False) and Teardown afterwards
        with stirling_settings(open_lnk_direct=False):
            with StirlingDriver(original_exe_path) as drv:
                drv.start()
                time.sleep(0.5)
                drv.open_file_via_dialog(lnk_file)
                time.sleep(0.5)
                drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        assert out_file.read_bytes() == payload_content, "Original Stirling should open the target file payload via File Open dialog"

    @pytest.mark.ported
    def test_ported_lnk_opened_via_commandline_opens_raw_binary(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling opens .lnk raw binary when passed via command line argument."""
        target_file = tmp_path / "p_actual_payload_cmd.dat"
        lnk_file = tmp_path / "p_shortcut_cmd.lnk"
        out_file = tmp_path / "p_out_cmd.dat"

        payload_content = b"PORTED_PAYLOAD_VIA_CMD_12345"
        target_file.write_bytes(payload_content)

        create_windows_shortcut(target_file, lnk_file)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(lnk_file)
            time.sleep(0.5)
            drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        assert saved_bytes.startswith(b"L\x00\x00\x00"), "Ported Stirling should open raw .lnk binary via command line"

    @pytest.mark.ported
    def test_ported_lnk_opened_via_open_dialog_resolves_target(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling resolves link target when opened via 'File - Open' dialog (default setting)."""
        target_file = tmp_path / "p_actual_payload_dlg.dat"
        lnk_file = tmp_path / "p_shortcut_dlg.lnk"
        out_file = tmp_path / "p_out_dlg.dat"

        payload_content = b"PORTED_PAYLOAD_VIA_OPEN_DIALOG_TARGET_67890"
        target_file.write_bytes(payload_content)

        create_windows_shortcut(target_file, lnk_file)

        # Setup: Ensure prerequisite setting (open_lnk_direct = False) and Teardown afterwards
        with stirling_settings(open_lnk_direct=False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                time.sleep(0.5)
                drv.open_file_via_dialog(lnk_file)
                time.sleep(0.5)
                drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        assert out_file.read_bytes() == payload_content, "Ported Stirling should resolve .lnk target via Open Dialog"
