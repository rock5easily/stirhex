import time
import pytest
from pathlib import Path
from drivers.stirling_driver import StirlingDriver

# Command IDs for the two character sets the driver has no helper for.
ID_CHARSET_EBCDIC = 32856
ID_CHARSET_EBCIDK = 32857
ID_CHARSET_SJIS = 32852

# 64 bytes exercising every branch of the char pane builder (BuildCharCells).
#   Row 0 (0x00-0x0F): ASCII, CP932 double-byte pairs, half-width katakana,
#                      broken pairs (lead byte + invalid trail), control bytes,
#                      and a lead byte at the row end so the pair straddles rows.
#   Row 1 (0x10-0x1F): the trail byte of that straddling pair, boundary lead
#                      bytes (0xE0), single-byte kana range ends (0xA1/0xDF) and
#                      high bytes that are never lead bytes (0xFD-0xFF).
#   Row 2 (0x20-0x2F): EUC-JP sequences (main plane pairs, 0x8E single shift,
#                      a broken pair, plane boundary values).
#   Row 3 (0x30-0x3F): UTF-16LE code units: BMP chars, a CP932-unmappable char
#                      (U+20AC) and a surrogate pair.
CHAR_PANE_DATA = bytes([
    # row 0
    0x41, 0x42, 0x43,        # "ABC"
    0x82, 0xA0,              # CP932 "a" (hiragana)
    0x8A, 0xBF,              # CP932 kanji
    0xB1, 0xB2,              # half-width katakana (single byte)
    0x82, 0x20,              # broken: lead byte + invalid trail (space)
    0x82, 0x7F,              # broken: lead byte + excluded trail 0x7F
    0x00, 0x1F,              # control bytes
    0x82,                    # lead byte at row end (pair straddles the row)
    # row 1
    0xA2,                    # trail byte of the straddling pair
    0xE0, 0x40,              # lead byte from the upper range + lowest trail
    0xA1, 0xDF,              # single-byte kana range ends
    0xFD, 0xFE, 0xFF,        # high bytes that are never lead bytes
    0x20, 0x7E,              # printable ASCII range ends
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35,   # "012345"
    # row 2 (EUC-JP oriented)
    0xA4, 0xA2,              # EUC-JP hiragana
    0xA4, 0xA4,              # EUC-JP hiragana
    0x8E, 0xB1,              # EUC-JP single shift (half-width katakana)
    0xA4, 0x20,              # broken EUC-JP pair
    0x41, 0x42,              # "AB"
    0xA1, 0xA1,              # EUC-JP ideographic space
    0xFE, 0xFE,              # EUC-JP plane boundary
    0xA4, 0x0A,              # lead byte + control
    # row 3 (UTF-16LE oriented)
    0x42, 0x30,              # U+3042
    0x44, 0x30,              # U+3044
    0x41, 0x00,              # U+0041 'A'
    0xAC, 0x20,              # U+20AC (not representable in CP932)
    0x3D, 0xD8, 0x00, 0xDE,  # surrogate pair
    0x42, 0x00,              # U+0042 'B'
    0x0A, 0x00,              # U+000A
])

# Windows whose first byte only *looks* like a trail byte. The preceding byte is a
#   lead byte, but the pair is not valid, so the original renders the first byte as
#   its own cell instead of shifting the row (InitialCarry must not report "mid character").
#   0x3F / 0x7F / 0xFD are outside the CP932 trail range (0x40-0xFC, 0x7F excluded).
CARRY_SJIS_DATA = bytes([
    0x41, 0x82, 0x3F, 0x42, 0x82, 0xFD, 0x43, 0x82,
    0x40, 0x44, 0x82, 0x7F, 0x45, 0x82, 0xA0, 0x46,
    0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E,
    0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56,
])
# Same for EUC-JP: a main plane lead byte followed by a byte outside 0xA1-0xFE, and
#   the 0x8E single shift followed by a byte outside the kana range 0xA1-0xDF.
CARRY_EUC_DATA = bytes([
    0xA4, 0x41, 0x42, 0xA4, 0xFF, 0x43, 0x8E, 0x41,
    0x8E, 0x8E, 0xB1, 0xA4, 0xA2, 0x44, 0x45, 0x46,
    0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E,
    0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56,
])

# File whose last byte is a CP932 lead byte (truncated multi-byte sequence).
TRUNCATED_LEAD_DATA = bytes([0x41, 0x42, 0x82, 0xA0, 0x43, 0x82])


class TestIssue42CharPane:
    """Issue #42: the char pane keeps rendering the edited bytes as CP932.

    These tests drive the dump image (WriteDumpImage). It shares the cell
    builder and the window-start handling with the screen (DrawCharColumn) and
    the printer (OnPrint) - BuildCharCells / InitialCarry - so a byte-identical
    dump pins down the byte layer for all three. The screen and the print
    preview themselves are compared against the original by hand (screenshots),
    since their output is pixels rather than bytes.
    """

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    @pytest.mark.parametrize(
        "charset_cmd",
        [
            pytest.param(32851, id="ascii"),
            pytest.param(32852, id="sjis"),
            pytest.param(32853, id="euc"),
            pytest.param(32854, id="unicode"),
            pytest.param(ID_CHARSET_EBCDIC, id="ebcdic"),
            pytest.param(ID_CHARSET_EBCIDK, id="ebcidk"),
        ],
    )
    def test_golden_dump_char_pane_per_charset(self, run_both_stirling, charset_cmd):
        """Dump the whole document in each character set and compare byte for byte."""

        def action(drv: StirlingDriver, out_path: Path):
            drv.post_command(charset_cmd)
            time.sleep(0.3)
            drv.save_dump_via_dialog(out_path)
            # Leave the persisted charset at the application default (SJIS).
            drv.post_command(ID_CHARSET_SJIS)
            time.sleep(0.2)

        orig_out, port_out = run_both_stirling(action, CHAR_PANE_DATA)

        assert len(orig_out) > 0, "Original dump output is empty!"
        assert orig_out == port_out, "Ported char pane dump does not match Original Stirling!"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_golden_dump_truncated_lead_byte(self, run_both_stirling):
        """A file ending with a lead byte must render the same as the original."""

        def action(drv: StirlingDriver, out_path: Path):
            drv.post_command(ID_CHARSET_SJIS)
            time.sleep(0.3)
            drv.save_dump_via_dialog(out_path)

        orig_out, port_out = run_both_stirling(action, TRUNCATED_LEAD_DATA)

        assert len(orig_out) > 0, "Original dump output is empty!"
        assert orig_out == port_out, "Ported truncated lead byte dump does not match Original!"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    @pytest.mark.parametrize(
        "charset_cmd,start_addr",
        [
            # 0x10 is the trail byte of the pair starting at 0x0F (row aligned).
            pytest.param(32852, "10", id="sjis-row-start-trail"),
            # 0x04 is the trail byte of the pair at 0x03 (mid row, needs padding too).
            pytest.param(32852, "04", id="sjis-mid-row-trail"),
            # 0x21 is the trail byte of the EUC-JP pair at 0x20.
            pytest.param(32853, "21", id="euc-trail"),
            # 0x31 is odd, i.e. the second byte of a UTF-16 code unit.
            pytest.param(32854, "31", id="unicode-odd-offset"),
        ],
    )
    def test_golden_dump_range_starting_mid_character(
        self, run_both_stirling, charset_cmd, start_addr
    ):
        """Dump a range whose first byte is in the middle of a character.

        This is the InitialCarry path: the leading partial byte is replaced by a
        single blank cell and consumed, so the char pane stays aligned with the
        hex column. With a selection present the dump dialog defaults to it.
        """

        def action(drv: StirlingDriver, out_path: Path):
            drv.post_command(charset_cmd)
            time.sleep(0.3)
            drv.select_range_dialog(start_addr=start_addr, end_addr="3F", is_hex=True)
            time.sleep(0.3)
            drv.save_dump_via_dialog(out_path)
            drv.post_command(ID_CHARSET_SJIS)
            time.sleep(0.2)

        orig_out, port_out = run_both_stirling(action, CHAR_PANE_DATA)

        assert len(orig_out) > 0, "Original dump output is empty!"
        assert orig_out == port_out, "Ported range dump does not match Original Stirling!"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    @pytest.mark.parametrize(
        "data,charset_cmd,start_addr",
        [
            # SJIS: byte after a lead byte, outside the valid trail range
            pytest.param(CARRY_SJIS_DATA, 32852, "02", id="sjis-trail-below-range"),
            pytest.param(CARRY_SJIS_DATA, 32852, "05", id="sjis-trail-above-range"),
            pytest.param(CARRY_SJIS_DATA, 32852, "0B", id="sjis-trail-7f-excluded"),
            # SJIS: the valid trail range ends (must shift the row)
            pytest.param(CARRY_SJIS_DATA, 32852, "08", id="sjis-trail-lowest"),
            pytest.param(CARRY_SJIS_DATA, 32852, "0E", id="sjis-trail-valid"),
            # EUC-JP: main plane lead followed by a byte outside 0xA1-0xFE
            pytest.param(CARRY_EUC_DATA, 32853, "01", id="euc-non-main-plane-trail"),
            pytest.param(CARRY_EUC_DATA, 32853, "04", id="euc-trail-above-range"),
            # EUC-JP: 0x8E single shift followed by a byte outside 0xA1-0xDF
            pytest.param(CARRY_EUC_DATA, 32853, "07", id="euc-shift-bad-trail"),
            pytest.param(CARRY_EUC_DATA, 32853, "09", id="euc-shift-then-shift"),
            # EUC-JP: 0x8E single shift with a valid kana trail (must shift the row)
            pytest.param(CARRY_EUC_DATA, 32853, "0A", id="euc-shift-valid-trail"),
        ],
    )
    def test_golden_dump_range_starting_on_invalid_trail(
        self, run_both_stirling, data, charset_cmd, start_addr
    ):
        """The window start only counts as "mid character" if the pair is valid.

        `InitialCarry` must apply the same pairing rules as `BuildCharCells`:
        a lead byte followed by an invalid trail does not form a character, so
        the byte at the window start is a cell of its own and the row must not
        be shifted. Getting this wrong moves every cell of the first row.
        """

        def action(drv: StirlingDriver, out_path: Path):
            drv.post_command(charset_cmd)
            time.sleep(0.3)
            drv.select_range_dialog(start_addr=start_addr, end_addr="1F", is_hex=True)
            time.sleep(0.3)
            drv.save_dump_via_dialog(out_path)
            drv.post_command(ID_CHARSET_SJIS)
            time.sleep(0.2)

        orig_out, port_out = run_both_stirling(action, data)

        assert len(orig_out) > 0, "Original dump output is empty!"
        assert orig_out == port_out, "Ported range dump does not match Original Stirling!"

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    @pytest.mark.parametrize(
        "charset_cmd,encoding",
        [
            pytest.param(32852, "cp932", id="sjis"),
            pytest.param(32853, "euc_jp", id="euc"),
            pytest.param(32854, "utf-16-le", id="unicode"),
        ],
    )
    def test_golden_replace_japanese_text(self, run_both_stirling, charset_cmd, encoding):
        """Replace All with Japanese text patterns (the wide -> byte boundary).

        The pattern typed into the dialog is wide text; the view encodes it into
        the document's character set (BuildTextBytes / EncodeText). The resulting
        bytes must match the original Stirling exactly.
        """
        text = "あいうあABCあ"
        data = text.encode(encoding)

        def action(drv: StirlingDriver, out_path: Path):
            drv.post_command(charset_cmd)
            time.sleep(0.3)
            drv.replace_all_dialog(
                search_str="あ",
                replace_str="漢",
                search_is_hex=False,
                replace_is_hex=False,
            )
            time.sleep(0.3)
            drv.save_as_via_dialog(out_path)
            drv.post_command(ID_CHARSET_SJIS)
            time.sleep(0.2)

        orig_out, port_out = run_both_stirling(action, data)

        assert len(orig_out) > 0, "Original output is empty!"
        assert orig_out != data, "Replace All did not change anything in the original!"
        assert orig_out == port_out, "Ported Japanese replace result differs from Original!"
