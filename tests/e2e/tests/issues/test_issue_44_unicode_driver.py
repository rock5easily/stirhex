"""Issue #44: e2e driver Unicode window-message coverage."""

from pathlib import Path

import pytest

from drivers.stirling_driver import (
    IDC_UM_AVAILABLE,
    StirlingDriver,
    _require_compatible_bitness,
)


class TestIssue44UnicodeDriver:
    """Exercise text-bearing control paths against ANSI and Unicode builds."""

    def test_rejects_x86_python_for_x64_target(self):
        with pytest.raises(RuntimeError, match="requires 64-bit Python"):
            _require_compatible_bitness(False, True)

        _require_compatible_bitness(False, False)
        _require_compatible_bitness(True, True)

    @pytest.mark.original
    def test_original_reads_ansi_common_control_text(
        self, original_exe_path, tmp_path
    ):
        test_file = tmp_path / "ansi_control_text.dat"
        test_file.write_bytes(bytes(range(64)))

        with StirlingDriver(original_exe_path) as drv:
            drv.start(test_file)

            status_text = "".join(drv.get_all_statusbar_text())
            assert "SHIFT-JIS" in status_text
            assert "\ufffd" not in status_text
            assert any(ord(ch) > 0x7F for ch in status_text)

            sheet, page = drv.open_user_menu_page()
            try:
                items = drv.listbox_texts(page, IDC_UM_AVAILABLE)
                assert items
                assert all("\ufffd" not in item for item in items)
            finally:
                drv.close_settings_sheet(sheet, accept=False)

    @pytest.mark.ported
    def test_ported_reads_wide_control_text(self, ported_exe_path, tmp_path):
        test_file = tmp_path / "日本語ファイル名.dat"
        test_file.write_bytes(bytes(range(64)))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)

            assert any(test_file.name in title for title in drv.get_mdi_child_titles())
            assert any("00000000" in text for text in drv.get_all_statusbar_text())

            drv.toggle_struct_bar(show=True)
            drv.select_struct_type(0)
            rows = drv.get_struct_list_texts()
            assert rows and all(len(row) == 3 for row in rows)

            sheet, page = drv.open_user_menu_page()
            try:
                items = drv.listbox_texts(page, IDC_UM_AVAILABLE)
                assert any(any(ord(ch) > 0x7F for ch in item) for item in items)
            finally:
                drv.close_settings_sheet(sheet, accept=False)

    @pytest.mark.golden
    @pytest.mark.ported
    @pytest.mark.original
    def test_wm_char_accepts_half_and_full_width(self, run_both_stirling):
        def action(drv: StirlingDriver, output_file: Path):
            drv.set_charset_sjis()
            drv.press_tab()
            drv.type_text_chars("あｲ")
            drv.save_as_via_dialog(output_file)

        original, ported = run_both_stirling(action, bytes(16))

        assert original.startswith("あｲ".encode("cp932"))
        assert ported == original

    @pytest.mark.ported
    def test_ported_wm_ime_char_accepts_unicode(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "wm_ime_char_input.dat"
        output_file = tmp_path / "IME入力結果.dat"
        test_file.write_bytes(bytes(16))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            drv.set_charset_sjis()
            drv.press_tab()
            drv.type_ime_chars("あｲ")
            drv.save_as_via_dialog(output_file)

        assert output_file.read_bytes().startswith("あｲ".encode("cp932"))

    @pytest.mark.ported
    def test_ported_wm_settext_accepts_japanese_search(
        self, ported_exe_path, tmp_path
    ):
        source_text = "あいうあ"
        test_file = tmp_path / "search_input.dat"
        output_file = tmp_path / "日本語検索結果.dat"
        test_file.write_bytes(source_text.encode("cp932"))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(test_file)
            drv.set_charset_sjis()
            drv.replace_all_dialog(
                search_str="あ",
                replace_str="漢",
                search_is_hex=False,
                replace_is_hex=False,
            )
            drv.save_as_via_dialog(output_file)

        assert output_file.read_bytes() == "漢いう漢".encode("cp932")

    @pytest.mark.ported
    def test_ported_open_dialog_accepts_japanese_path(
        self, ported_exe_path, tmp_path
    ):
        test_file = tmp_path / "日本語フォルダ" / "日本語ファイル.dat"
        test_file.parent.mkdir()
        test_file.write_bytes(b"UNICODE_OPEN_DIALOG")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            drv.open_file_via_dialog(test_file)
            assert any(
                test_file.name in title for title in drv.get_mdi_child_titles()
            )
