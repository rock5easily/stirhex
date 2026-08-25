import time
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver
from drivers.settings_context import stirling_settings


class TestIssue10DynamicMark:
    """Tests for Issue #10: Dynamic Mark feature.
    
    Prerequisite settings:
    - '環境設定' - '編集動作2' -> 'ダイナミックマーク':
      - Enabled (dynamic_mark = True): Mark positions shift/follow upon insertion/deletion.
      - Disabled (dynamic_mark = False): Mark positions remain at fixed static addresses.
    """

    @pytest.mark.original
    def test_original_dynamic_mark_shifts_on_insert(self, original_exe_path, tmp_path):
        """Verify Original Stirling shifts mark position when data is inserted before mark (dynamic_mark = True)."""
        test_file = tmp_path / "dyn_mark_test_orig.dat"
        out_file = tmp_path / "dyn_mark_out_orig.dat"
        # 32 bytes: 0x00 .. 0x1F
        test_data = bytes(range(32))
        test_file.write_bytes(test_data)

        # Setup: Enable dynamic mark setting in HKCU\Software\DDS2\Stirling\Settings
        with stirling_settings(dynamic_mark=True):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()

                # 1. Jump to offset 8 (value 0x08) and set mark
                drv.jump_to_address("8", is_hex=True)
                time.sleep(0.2)
                drv.mark_toggle()
                time.sleep(0.2)

                # 2. Jump to offset 0, switch to Insert mode, and insert 4 bytes (0xAA, 0xBB, 0xCC, 0xDD)
                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.2)
                drv.press_insert()
                time.sleep(0.2)
                drv.type_hex_chars("AABBCCDD")
                time.sleep(0.2)

                # 3. Move back to top and jump to next mark
                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.2)
                drv.mark_next()
                time.sleep(0.2)

                # 4. Delete the byte at mark position (should be the original 0x08 at shifted offset 12)
                drv.press_delete()
                time.sleep(0.2)

                drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        # Original 32 bytes + 4 inserted - 1 deleted = 35 bytes
        assert len(saved_bytes) == 35
        # Verify 0x08 was deleted from the shifted position
        assert 0x08 not in saved_bytes, "Dynamic mark did not follow data insertion to offset 12"

    @pytest.mark.original
    def test_original_static_mark_fixed_on_insert(self, original_exe_path, tmp_path):
        """Verify Original Stirling keeps mark at static address when dynamic mark is disabled (dynamic_mark = False)."""
        test_file = tmp_path / "static_mark_test_orig.dat"
        out_file = tmp_path / "static_mark_out_orig.dat"
        test_data = bytes(range(32))
        test_file.write_bytes(test_data)

        # Setup: Disable dynamic mark setting
        with stirling_settings(dynamic_mark=False):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()

                # 1. Jump to offset 8 and set mark
                drv.jump_to_address("8", is_hex=True)
                time.sleep(0.2)
                drv.mark_toggle()
                time.sleep(0.2)

                # 2. Jump to offset 0, insert 4 bytes
                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.2)
                drv.press_insert()
                time.sleep(0.2)
                drv.type_hex_chars("AABBCCDD")
                time.sleep(0.2)

                # 3. Move back to top and jump to next mark
                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.2)
                drv.mark_next()
                time.sleep(0.2)

                # 4. Delete the byte at mark position (should delete byte at fixed offset 8, which is 0x04 after shift)
                drv.press_delete()
                time.sleep(0.2)

                drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        assert len(saved_bytes) == 35
        assert 0x04 not in saved_bytes, "Static mark did not stay at fixed address 8"
        assert 0x08 in saved_bytes

    @pytest.mark.ported
    def test_ported_dynamic_mark_shifts_on_insert(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling shifts mark position when data is inserted before mark (dynamic_mark = True)."""
        test_file = tmp_path / "dyn_mark_test_port.dat"
        out_file = tmp_path / "dyn_mark_out_port.dat"
        test_data = bytes(range(32))
        test_file.write_bytes(test_data)

        with stirling_settings(dynamic_mark=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()

                # 1. Jump to offset 8 and set mark
                drv.jump_to_address("8", is_hex=True)
                time.sleep(0.2)
                drv.mark_toggle()
                time.sleep(0.2)

                # 2. Jump to offset 0, insert 4 bytes
                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.2)
                drv.press_insert()
                time.sleep(0.2)
                drv.type_hex_chars("AABBCCDD")
                time.sleep(0.2)

                # 3. Jump to next mark
                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.2)
                drv.mark_next()
                time.sleep(0.2)

                # 4. Delete at mark position
                drv.press_delete()
                time.sleep(0.2)

                drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        assert len(saved_bytes) == 35
        assert 0x08 not in saved_bytes, "Ported Stirling dynamic mark did not follow data insertion to offset 12"

    @pytest.mark.ported
    def test_ported_static_mark_fixed_on_insert(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling keeps mark at static address when dynamic mark is disabled (dynamic_mark = False)."""
        test_file = tmp_path / "static_mark_test_port.dat"
        out_file = tmp_path / "static_mark_out_port.dat"
        test_data = bytes(range(32))
        test_file.write_bytes(test_data)

        with stirling_settings(dynamic_mark=False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()

                drv.jump_to_address("8", is_hex=True)
                time.sleep(0.2)
                drv.mark_toggle()
                time.sleep(0.2)

                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.2)
                drv.press_insert()
                time.sleep(0.2)
                drv.type_hex_chars("AABBCCDD")
                time.sleep(0.2)

                drv.jump_to_address("0", is_hex=True)
                time.sleep(0.2)
                drv.mark_next()
                time.sleep(0.2)

                drv.press_delete()
                time.sleep(0.2)

                drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        saved_bytes = out_file.read_bytes()
        assert len(saved_bytes) == 35
        assert 0x04 not in saved_bytes
        assert 0x08 in saved_bytes
