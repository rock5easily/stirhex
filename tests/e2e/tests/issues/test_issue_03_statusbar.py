import time
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver
from drivers.settings_context import stirling_settings

# A repeating, asymmetric sequence: every detail pane reads different bytes depending on
# the caret position and the byte order, so a wrong reading cannot look right by accident.
DETAIL_DATA = bytes([0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0] * 4)

# Status bar layout with the detail panes (command ids of the status item catalogue).
#   stirling_settings stores this in whichever form each build reads (Issue #211), so the
#   same layout can be given to the original and to the port.
DETAIL_ITEMS = [
    0xE709,  # address (hex)
    0xE713,  # BYTE (hex)
    0xE70B,  # WORD (hex)
    0xE70C,  # DWORD (hex)
    0xE714,  # float
    0xE715,  # double
    0xE716,  # byte order
]


def _detail_panes(drv: StirlingDriver, address: str) -> dict:
    """Jump to `address` and read the detail panes as {"B": "0x12", "W": ..., ...}."""
    drv.jump_to_address(address, is_hex=True)
    time.sleep(0.5)
    panes = drv.get_all_statusbar_text()
    values = {}
    for text in panes:
        if " : " in text:
            label, value = text.split(" : ", 1)
            values[label.strip()] = value.strip()
        elif text.endswith("Endian"):
            values["order"] = text.strip()
    assert {"B", "order"} <= set(values), f"unexpected panes: {panes}"
    # 値の入らないペインは空文字列で返す。原版は読み出すバイトが足りないペインを
    # 空欄にする（移植版は不足分を 0 として表示する。Issue #212）。
    for label in ("W", "DW", "f", "d"):
        values.setdefault(label, "")
    return values


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

    # --- Detail panes: the values themselves, in both byte orders (Issue #204) ---
    #
    # The cases above only prove that seven panes exist. These configure the detail items
    # and check what they actually show for a known byte sequence, which is where byte
    # order becomes observable at all: the previous golden "byteorder toggle" case typed
    # hex digits, and those land on the same bytes whichever order is set.

    @pytest.mark.ported
    @pytest.mark.parametrize(
        ("byte_order", "word_at_0", "dword_at_0", "word_at_1", "dword_at_1"),
        [
            ("little", "0x3412", "0x78563412", "0x5634", "0x9A785634"),
            ("big", "0x1234", "0x12345678", "0x3456", "0x3456789A"),
        ],
    )
    def test_ported_statusbar_detail_values(
        self,
        ported_exe_path,
        tmp_path,
        byte_order,
        word_at_0,
        dword_at_0,
        word_at_1,
        dword_at_1,
    ):
        """WORD / DWORD panes must show the bytes under the caret in the selected order."""
        test_file = tmp_path / f"statusbar_detail_{byte_order}.dat"
        test_file.write_bytes(DETAIL_DATA)

        with stirling_settings(show_status_bar=True, status_items=DETAIL_ITEMS):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.4)
                if byte_order == "big":
                    drv.set_byteorder_big()
                else:
                    drv.set_byteorder_little()
                time.sleep(0.4)

                at_zero = _detail_panes(drv, "0")
                assert at_zero["B"] == "0x12", f"BYTE pane: {at_zero}"
                assert at_zero["W"] == word_at_0, f"WORD pane: {at_zero}"
                assert at_zero["DW"] == dword_at_0, f"DWORD pane: {at_zero}"
                assert at_zero["order"] == (
                    "BigEndian" if byte_order == "big" else "LittleEndian"
                )

                # Moving the caret one byte shifts the window the panes read from.
                at_one = _detail_panes(drv, "1")
                assert at_one["B"] == "0x34", f"BYTE pane at 0x01: {at_one}"
                assert at_one["W"] == word_at_1, f"WORD pane at 0x01: {at_one}"
                assert at_one["DW"] == dword_at_1, f"DWORD pane at 0x01: {at_one}"

                # Near the end of the file a pane stays empty unless the bytes it needs
                # are really there; the port used to pad the missing ones with zero and
                # show a value that the file does not contain (Issue #212).
                at_eof = _detail_panes(drv, "1F")
                assert at_eof["B"] == "0xF0", f"BYTE pane at EOF: {at_eof}"
                assert at_eof["W"] == "", f"WORD pane at EOF: {at_eof}"
                assert at_eof["DW"] == "", f"DWORD pane at EOF: {at_eof}"
                assert at_eof["f"] == "", f"float pane at EOF: {at_eof}"
                assert at_eof["d"] == "", f"double pane at EOF: {at_eof}"

                # Three bytes left: WORD still fits, DWORD and the floats do not.
                three_left = _detail_panes(drv, "1D")
                assert three_left["W"] != "", f"WORD fits in 3 bytes: {three_left}"
                assert three_left["DW"] == "", f"DWORD needs 4 bytes: {three_left}"
                assert three_left["f"] == "", f"float needs 4 bytes: {three_left}"

    @pytest.mark.ported
    def test_ported_statusbar_float_panes_follow_byte_order(self, ported_exe_path, tmp_path):
        """float / double panes must read the same bytes in the selected order."""
        test_file = tmp_path / "statusbar_float.dat"
        test_file.write_bytes(DETAIL_DATA)

        readings = {}
        for byte_order in ("little", "big"):
            with stirling_settings(show_status_bar=True, status_items=DETAIL_ITEMS):
                with StirlingDriver(ported_exe_path) as drv:
                    drv.start(test_file)
                    drv.focus_view()
                    time.sleep(0.4)
                    if byte_order == "big":
                        drv.set_byteorder_big()
                    else:
                        drv.set_byteorder_little()
                    time.sleep(0.4)
                    readings[byte_order] = _detail_panes(drv, "0")

        for pane in ("f", "d"):
            little = readings["little"][pane]
            big = readings["big"][pane]
            assert little and big, f"the {pane} pane must show a value: {readings}"
            assert little != big, (
                f"the {pane} pane shows {little} in both byte orders, so it ignores the setting"
            )

        # The double pane keeps the precision the original shows (15 significant digits,
        # measured as -4.88645965504377e+235). The default %g would cut it to 6 and lose
        # the low digits (Issue #212).
        assert readings["little"]["d"] == "-4.88645965504377e+235", (
            f"double precision differs from the original: {readings['little']['d']}"
        )

    # --- The original with the same layout (Issue #211) ---
    #
    # The original keeps the pane layout in one REG_BINARY ("StatusBar"), so it could not
    # be configured from the tests and the detail values were checked on the port only.
    # With stirling_settings able to write both forms, the same expectations can be put
    # to the original - which is what makes them a statement about compatibility rather
    # than about the port alone.

    @pytest.mark.original
    @pytest.mark.parametrize(
        ("byte_order", "word_at_0", "dword_at_0"),
        [
            ("little", "0x3412", "0x78563412"),
            ("big", "0x1234", "0x12345678"),
        ],
    )
    def test_original_statusbar_detail_values(
        self, original_exe_path, tmp_path, byte_order, word_at_0, dword_at_0
    ):
        """The original shows the same BYTE / WORD / DWORD values in each byte order."""
        test_file = tmp_path / f"statusbar_orig_{byte_order}.dat"
        test_file.write_bytes(DETAIL_DATA)

        with stirling_settings(show_status_bar=True, status_items=DETAIL_ITEMS):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.4)
                if byte_order == "big":
                    drv.set_byteorder_big()
                else:
                    drv.set_byteorder_little()
                time.sleep(0.4)

                panes = _detail_panes(drv, "0")
                assert panes["B"] == "0x12", f"BYTE pane: {panes}"
                assert panes["W"] == word_at_0, f"WORD pane: {panes}"
                assert panes["DW"] == dword_at_0, f"DWORD pane: {panes}"
                assert panes["order"] == (
                    "BigEndian" if byte_order == "big" else "LittleEndian"
                )

    @pytest.mark.original
    def test_original_statusbar_blanks_panes_without_enough_bytes(
        self, original_exe_path, tmp_path
    ):
        """The original leaves a pane empty when the file has too few bytes left.

        Measured on Stirling 1.31 with a 32 byte file (Issue #211):

            0x1C (4 bytes left) : B / W / DW / float shown, double empty
            0x1D (3 bytes left) : B / W shown, the rest empty
            0x1F (last byte)    : B only

        The port instead pads the missing bytes with zero and always shows a value; that
        difference is Issue #212. This case records the original as the reference.
        """
        test_file = tmp_path / "statusbar_orig_eof.dat"
        test_file.write_bytes(DETAIL_DATA)

        with stirling_settings(show_status_bar=True, status_items=DETAIL_ITEMS):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()
                time.sleep(0.4)
                drv.set_byteorder_little()
                time.sleep(0.4)

                four_left = _detail_panes(drv, "1C")
                assert four_left["B"] == "0x9A", f"@0x1C: {four_left}"
                assert four_left["W"] == "0xBC9A", f"@0x1C: {four_left}"
                assert four_left["DW"] == "0xF0DEBC9A", f"@0x1C: {four_left}"
                assert four_left["f"], f"float fits in 4 bytes: {four_left}"
                assert four_left["d"] == "", f"double needs 8 bytes: {four_left}"

                three_left = _detail_panes(drv, "1D")
                assert three_left["W"] == "0xDEBC", f"@0x1D: {three_left}"
                assert three_left["DW"] == "", f"DWORD needs 4 bytes: {three_left}"
                assert three_left["f"] == "", f"float needs 4 bytes: {three_left}"

                last = _detail_panes(drv, "1F")
                assert last["B"] == "0xF0", f"@0x1F: {last}"
                assert last["W"] == "", f"WORD needs 2 bytes: {last}"
                assert last["DW"] == "", f"DWORD needs 4 bytes: {last}"
