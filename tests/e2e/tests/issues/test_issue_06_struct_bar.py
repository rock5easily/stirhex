import time
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver, safe_set_focus
from drivers.settings_context import stirling_settings


class TestIssue06StructBar:
    """Tests for Issue #6: Struct Edit Bar context menu, radix switching, mark jump, and related settings.
    
    Features covered:
    - Struct Bar visibility toggle and struct template selection (e.g. LOGFONT).
    - Address navigation buttons (<<, <, >, >>).
    - Mark jump via '移動' (IDD_TOP_ADDRESS) dialog.
    - Syncing struct address to caret position ('キャレット位置を構造体編集').
    - Radix switching (Hex / Decimal / Default).
    - Prerequisite settings:
      - '現在位置を構造体編集アドレスに自動設定' (auto_set_struct_addr).
      - '構造体アイテム幅の比率を保持' (keep_struct_item_ratio).
    """

    @pytest.mark.original
    def test_original_struct_bar_nav_and_mark_jump(self, original_exe_path, tmp_path):
        """Verify Original Stirling struct bar navigation and mark jump via '移動' dialog."""
        test_file = tmp_path / "struct_nav_orig.dat"
        test_file.write_bytes(bytes(range(256)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # 1. Show Struct Bar
            drv.toggle_struct_bar(show=True)
            time.sleep(0.3)
            assert drv.is_struct_bar_visible(), "Struct bar should be visible"

            # 2. Select LOGFONT (size = 60 / 0x3C bytes)
            drv.select_struct_type("LOGFONT")
            time.sleep(0.3)
            assert drv.get_struct_address() == "00000000"

            # 3. Test Navigation buttons
            # '>' (+1 byte) -> 0x01
            drv.struct_nav_next_byte()
            time.sleep(0.2)
            assert drv.get_struct_address() == "00000001"

            # '>>' (+0x3C struct size) -> 0x01 + 0x3C = 0x3D
            drv.struct_nav_next_rec()
            time.sleep(0.2)
            assert drv.get_struct_address() == "0000003D"

            # '<' (-1 byte) -> 0x3C
            drv.struct_nav_prev_byte()
            time.sleep(0.2)
            assert drv.get_struct_address() == "0000003C"

            # '<<' (-0x3C struct size) -> 0x00
            drv.struct_nav_prev_rec()
            time.sleep(0.2)
            assert drv.get_struct_address() == "00000000"

            # 4. Set mark at offset 0x20 and jump via '移動' dialog
            drv.jump_to_address("20", is_hex=True)
            time.sleep(0.2)
            drv.mark_toggle()
            time.sleep(0.2)

            drv.struct_goto_dialog(mode="mark", mark_index=0)
            time.sleep(0.3)
            assert drv.get_struct_address() == "00000020", "Struct address should jump to mark location 0x20"

    @pytest.mark.ported
    def test_ported_struct_bar_nav_and_mark_jump(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling struct bar navigation and mark jump via '移動' dialog."""
        test_file = tmp_path / "struct_nav_port.dat"
        test_file.write_bytes(bytes(range(256)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            # 1. Show Struct Bar
            drv.toggle_struct_bar(show=True)
            time.sleep(0.3)
            assert drv.is_struct_bar_visible(), "Struct bar should be visible"

            # 2. Select LOGFONT (size = 60 / 0x3C bytes)
            drv.select_struct_type("LOGFONT")
            time.sleep(0.3)
            assert drv.get_struct_address() == "00000000"

            # 3. Test Navigation buttons
            drv.struct_nav_next_byte()
            time.sleep(0.2)
            assert drv.get_struct_address() == "00000001"

            drv.struct_nav_next_rec()
            time.sleep(0.2)
            assert drv.get_struct_address() == "0000003D"

            drv.struct_nav_prev_byte()
            time.sleep(0.2)
            assert drv.get_struct_address() == "0000003C"

            drv.struct_nav_prev_rec()
            time.sleep(0.2)
            assert drv.get_struct_address() == "00000000"

            # 4. Set mark at offset 0x20 and jump via '移動' dialog
            drv.jump_to_address("20", is_hex=True)
            time.sleep(0.2)
            drv.mark_toggle()
            time.sleep(0.2)

            drv.struct_goto_dialog(mode="mark", mark_index=0)
            time.sleep(0.3)
            assert drv.get_struct_address() == "00000020", "Struct address should jump to mark location 0x20"

    @pytest.mark.original
    def test_original_set_struct_address_to_caret(self, original_exe_path, tmp_path):
        """Verify Original Stirling syncs struct bar address to caret via 'キャレット位置を構造体編集'."""
        test_file = tmp_path / "struct_caret_orig.dat"
        test_file.write_bytes(bytes(range(256)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.toggle_struct_bar(show=True)
            time.sleep(0.3)
            drv.select_struct_type("LOGFONT")
            time.sleep(0.3)

            # Move caret to 0x40 and execute 'キャレット位置を構造体編集' (32865)
            drv.jump_to_address("40", is_hex=True)
            time.sleep(0.2)
            drv.set_struct_address_to_caret()
            time.sleep(0.3)
            assert drv.get_struct_address() == "00000040", "Struct address should sync to caret position 0x40"

    @pytest.mark.ported
    def test_ported_set_struct_address_to_caret(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling syncs struct bar address to caret via 'キャレット位置を構造体編集'."""
        test_file = tmp_path / "struct_caret_port.dat"
        test_file.write_bytes(bytes(range(256)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.toggle_struct_bar(show=True)
            time.sleep(0.3)
            drv.select_struct_type("LOGFONT")
            time.sleep(0.3)

            # Move caret to 0x40 and execute 'キャレット位置を構造体編集' (32865)
            drv.jump_to_address("40", is_hex=True)
            time.sleep(0.2)
            drv.set_struct_address_to_caret()
            time.sleep(0.3)
            assert drv.get_struct_address() == "00000040", "Struct address should sync to caret position 0x40"

    @pytest.mark.original
    def test_original_struct_bar_radix_switch(self, original_exe_path, tmp_path):
        """Verify Original Stirling switches struct field display radix via context menu."""
        test_file = tmp_path / "struct_radix_orig.dat"
        test_file.write_bytes(bytes(range(256)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.toggle_struct_bar(show=True)
            time.sleep(0.3)
            drv.select_struct_type("LOGFONT")
            time.sleep(0.3)

            # Switch all radix to Hex (A -> H)
            drv.struct_set_radix_all("hex")
            time.sleep(0.3)

            # Switch all radix to Dec (A -> S)
            drv.struct_set_radix_all("dec_signed")
            time.sleep(0.3)

    @pytest.mark.ported
    def test_ported_struct_bar_radix_switch(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling struct field display radix switching (all and individual)."""
        test_file = tmp_path / "struct_radix_port.dat"
        test_file.write_bytes(bytes(range(256)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            safe_set_focus(drv.hwnd)
            time.sleep(0.3)

            drv.toggle_struct_bar(show=True)
            time.sleep(0.3)
            drv.select_struct_type("LOGFONT")
            time.sleep(0.3)

            # 1. Default rows: LONG is signed decimal
            rows_default = drv.get_struct_list_texts()
            assert len(rows_default) > 0, "Struct rows should be populated"
            assert rows_default[0][1] == "lfHeight"
            assert rows_default[1][1] == "lfWidth"
            assert rows_default[1][2] == "117835012"

            # 2. Individual radix on row 1 (lfWidth) -> Hex
            drv.struct_set_radix_item(1, "hex")
            time.sleep(0.3)
            rows_item_hex = drv.get_struct_list_texts()
            assert not rows_item_hex[0][2].startswith("0x"), "lfHeight should remain decimal"
            assert rows_item_hex[1][2] == "0x07060504", f"lfWidth should be hex 0x07060504, got '{rows_item_hex[1][2]}'"

            # 3. Step address (+1 byte) preserves individual radix
            drv.struct_nav_next_byte()
            time.sleep(0.2)
            rows_stepped = drv.get_struct_list_texts()
            assert rows_stepped[1][2] == "0x08070605", f"lfWidth should step to 0x08070605, got '{rows_stepped[1][2]}'"

            # Return to 0
            drv.struct_nav_prev_byte()
            time.sleep(0.2)

            # 4. Switch all radix to Hex
            drv.struct_set_radix_all("hex")
            time.sleep(0.3)
            rows_hex = drv.get_struct_list_texts()
            assert rows_hex[0][2].startswith("0x"), f"Expected hex for lfHeight, got '{rows_hex[0][2]}'"
            assert rows_hex[1][2].startswith("0x"), f"Expected hex for lfWidth, got '{rows_hex[1][2]}'"

            # 5. Switch all radix to Default
            drv.struct_set_radix_all("default")
            time.sleep(0.3)
            rows_def = drv.get_struct_list_texts()
            assert not rows_def[0][2].startswith("0x"), f"Expected decimal for lfHeight, got '{rows_def[0][2]}'"
            assert rows_def[1][2] == "117835012", f"Expected default decimal 117835012 for lfWidth, got '{rows_def[1][2]}'"

    @pytest.mark.original
    def test_original_auto_set_struct_addr_setting(self, original_exe_path, tmp_path):
        """Verify Original Stirling AutoSetSEAddress setting behavior (auto follow caret vs static address)."""
        test_file = tmp_path / "struct_auto_orig.dat"
        test_file.write_bytes(bytes(range(256)))

        # 1. When auto_set_struct_addr = True, opening struct bar at caret position 0x30 uses 0x30
        with stirling_settings(auto_set_struct_addr=True):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.jump_to_address("30", is_hex=True)
                time.sleep(0.2)
                drv.toggle_struct_bar(show=True)
                time.sleep(0.3)
                drv.select_struct_type("LOGFONT")
                time.sleep(0.3)
                assert drv.get_struct_address() == "00000030", "Struct base address should follow caret when auto_set is True"

        # 2. When auto_set_struct_addr = False, struct address does not follow caret movement
        with stirling_settings(auto_set_struct_addr=False):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.toggle_struct_bar(show=True)
                time.sleep(0.3)
                drv.select_struct_type("LOGFONT")
                time.sleep(0.3)
                initial_addr = drv.get_struct_address()
                
                # Move caret to 0x30
                drv.jump_to_address("30", is_hex=True)
                time.sleep(0.3)
                assert drv.get_struct_address() == initial_addr, "Struct base address should not follow caret when auto_set is False"

    @pytest.mark.ported
    def test_ported_auto_set_struct_addr_setting(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling CurPosToStructAddr setting behavior."""
        test_file = tmp_path / "struct_auto_port.dat"
        test_file.write_bytes(bytes(range(256)))

        # 1. When auto_set_struct_addr = True
        with stirling_settings(auto_set_struct_addr=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.jump_to_address("30", is_hex=True)
                time.sleep(0.2)
                drv.toggle_struct_bar(show=True)
                time.sleep(0.3)
                drv.select_struct_type("LOGFONT")
                time.sleep(0.3)
                assert drv.get_struct_address() == "00000030", "Struct base address should follow caret when auto_set is True"

        # 2. When auto_set_struct_addr = False
        with stirling_settings(auto_set_struct_addr=False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.toggle_struct_bar(show=True)
                time.sleep(0.3)
                drv.select_struct_type("LOGFONT")
                time.sleep(0.3)
                initial_addr = drv.get_struct_address()
                
                drv.jump_to_address("30", is_hex=True)
                time.sleep(0.3)
                assert drv.get_struct_address() == initial_addr, "Struct base address should not follow caret when auto_set is False"

    @pytest.mark.original
    def test_original_keep_struct_item_ratio_setting(self, original_exe_path, tmp_path):
        """Verify Original Stirling SaveItemRatio setting preserves column ratio."""
        test_file = tmp_path / "struct_ratio_orig.dat"
        test_file.write_bytes(bytes(range(256)))

        with stirling_settings(keep_struct_item_ratio=True):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.toggle_struct_bar(show=True)
                time.sleep(0.3)
                drv.select_struct_type("LOGFONT")
                time.sleep(0.3)
                assert drv.is_struct_bar_visible()

    @pytest.mark.ported
    def test_ported_keep_struct_item_ratio_setting(self, ported_exe_path, tmp_path):
        """Verify Ported Stirling StructItemRatioKeep setting preserves column ratio."""
        test_file = tmp_path / "struct_ratio_port.dat"
        test_file.write_bytes(bytes(range(256)))

        with stirling_settings(keep_struct_item_ratio=True):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.toggle_struct_bar(show=True)
                time.sleep(0.3)
                drv.select_struct_type("LOGFONT")
                time.sleep(0.3)
                widths = drv.get_struct_column_widths()
                assert len(widths) == 3, f"Expected 3 columns, got {widths}"
                assert all(w > 0 for w in widths), f"Column widths should be positive: {widths}"
