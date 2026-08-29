"""Issue #27: 環境設定「ユーザーメニュー」でアクセラレータをダイアログ指定する。

原版は「追加」ボタンおよび項目のダブルクリックで「アクセラレータの指定」ダイアログ
(IDD_ACCEL_INPUT 184) を表示し、ユーザーが 1 文字を割り当てる。移植版は追加順の
A, B, C... 自動割当だったため、任意キーの指定も既存項目の変更もできなかった。
"""

import time

import pytest

from drivers.stirling_driver import (
    StirlingDriver,
    safe_set_focus,
    IDC_UM_AVAILABLE,
    IDC_UM_CURRENT,
)
from drivers.settings_context import read_reg_values, stirling_settings


def _make_file(tmp_path, name: str):
    p = tmp_path / name
    p.write_bytes(bytes(range(64)))
    return p


class TestIssue27AccelInput:
    """アクセラレータ指定ダイアログの表示・入力・反映を検証する。"""

    # 1. 「追加」でアクセラレータ指定ダイアログが出る（原版・移植版の双方）
    @pytest.mark.original
    def test_original_add_shows_accel_dialog(self, original_exe_path, tmp_path):
        """原版: 「追加」でアクセラレータの指定ダイアログが表示される。"""
        test_file = _make_file(tmp_path, "accel_orig.dat")

        with stirling_settings(user_menus={0: []}):
            with StirlingDriver(original_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                sheet, page = drv.open_user_menu_page()
                drv.um_select_available(page, 0)
                drv.um_click_add(page)

                dlg = drv.find_accel_dialog()
                assert dlg is not None, "原版でアクセラレータの指定ダイアログが出ない"
                drv.accel_dialog_close(dlg, accept=False)
                drv.close_settings_sheet(sheet, accept=False)

    @pytest.mark.ported
    def test_ported_add_shows_accel_dialog(self, ported_exe_path, tmp_path):
        """移植版: 「追加」でアクセラレータの指定ダイアログが表示される。"""
        test_file = _make_file(tmp_path, "accel_port.dat")

        with stirling_settings(user_menus={0: []}):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                sheet, page = drv.open_user_menu_page()
                drv.um_select_available(page, 0)
                drv.um_click_add(page)

                dlg = drv.find_accel_dialog()
                assert dlg is not None, "移植版でアクセラレータの指定ダイアログが出ない"
                # 追加モードは空欄で始まり、空欄の間 OK は無効（原 FUN_004014e8 / FUN_00401570）。
                assert drv.accel_dialog_text(dlg) == "", "追加モードの初期値は空欄であるべき"
                assert not drv.accel_dialog_ok_enabled(dlg), "空欄では OK が無効であるべき"
                drv.accel_dialog_close(dlg, accept=False)
                drv.close_settings_sheet(sheet, accept=False)

    # 2. 指定した任意キーが割り当てられる（自動 A, B, C... ではない）
    @pytest.mark.ported
    def test_ported_typed_accel_is_assigned(self, ported_exe_path, tmp_path):
        """移植版: 入力した文字が大文字化されて項目のアクセラレータになる。"""
        test_file = _make_file(tmp_path, "accel_assign.dat")

        with stirling_settings(user_menus={0: []}):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                sheet, page = drv.open_user_menu_page()
                item_name = drv.listbox_texts(page, IDC_UM_AVAILABLE)[0]
                drv.um_select_available(page, 0)
                drv.um_click_add(page)

                dlg = drv.find_accel_dialog()
                assert dlg is not None
                drv.accel_dialog_type(dlg, "q")          # ES_UPPERCASE で 'Q' になる
                assert drv.accel_dialog_text(dlg) == "Q", "ES_UPPERCASE で大文字化されるべき"
                assert drv.accel_dialog_ok_enabled(dlg), "1文字入力で OK が有効になるべき"
                drv.accel_dialog_close(dlg, accept=True)

                current = drv.listbox_texts(page, IDC_UM_CURRENT)
                assert len(current) == 1, f"項目が追加されていない: {current}"
                assert current[0].startswith("Q"), \
                    f"指定した 'Q' が割り当てられていない（自動割当のまま？）: {current[0]}"
                assert item_name in current[0], f"項目名が一致しない: {current[0]} / {item_name}"
                drv.close_settings_sheet(sheet, accept=False)

    # 3. 2 文字目は入れられない（LimitText(1)）
    @pytest.mark.ported
    def test_ported_accel_limited_to_one_char(self, ported_exe_path, tmp_path):
        """移植版: エディットは 1 文字までしか受け付けない。"""
        test_file = _make_file(tmp_path, "accel_limit.dat")

        with stirling_settings(user_menus={0: []}):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                sheet, page = drv.open_user_menu_page()
                drv.um_select_available(page, 0)
                drv.um_click_add(page)

                dlg = drv.find_accel_dialog()
                assert dlg is not None
                drv.accel_dialog_type(dlg, "a")
                drv.accel_dialog_type(dlg, "b")
                assert drv.accel_dialog_text(dlg) == "A", "1 文字制限が効いていない"
                drv.accel_dialog_close(dlg, accept=False)
                drv.close_settings_sheet(sheet, accept=False)

    # 4. キャンセルすると項目自体が追加されない（原と同じ）
    @pytest.mark.ported
    def test_ported_cancel_skips_add(self, ported_exe_path, tmp_path):
        """移植版: アクセラレータ指定をキャンセルすると項目は追加されない。"""
        test_file = _make_file(tmp_path, "accel_cancel.dat")

        with stirling_settings(user_menus={0: []}):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                sheet, page = drv.open_user_menu_page()
                drv.um_select_available(page, 0)
                drv.um_click_add(page)

                dlg = drv.find_accel_dialog()
                assert dlg is not None
                drv.accel_dialog_close(dlg, accept=False)

                assert drv.listbox_texts(page, IDC_UM_CURRENT) == [], \
                    "キャンセルしたのに項目が追加されている"
                drv.close_settings_sheet(sheet, accept=False)

    # 5. 既存項目のダブルクリックでアクセラレータを変更できる（原 FUN_0042c462）
    @pytest.mark.ported
    def test_ported_dblclk_changes_accel(self, ported_exe_path, tmp_path):
        """移植版: 現在のメニュー設定の項目をダブルクリックしてアクセラレータを変更できる。"""
        test_file = _make_file(tmp_path, "accel_change.dat")

        # メニュー1 に「切り取り」(rawID 0x0302, アクセラレータ 'T') を 1 件用意する。
        with stirling_settings(user_menus={0: [(ord("T") << 16) | 0x0302]}):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                sheet, page = drv.open_user_menu_page()
                before = drv.listbox_texts(page, IDC_UM_CURRENT)
                assert len(before) == 1 and before[0].startswith("T"), f"前提が崩れている: {before}"

                drv.um_dblclk_current(page, 0)
                dlg = drv.find_accel_dialog()
                assert dlg is not None, "ダブルクリックでアクセラレータの指定ダイアログが出ない"
                # 変更モードは現在値を表示する（移植版の改善。原はここが空欄だった）。
                assert drv.accel_dialog_text(dlg) == "T", "現在のアクセラレータが表示されていない"
                assert drv.accel_dialog_ok_enabled(dlg), "変更モードの OK は初期から有効であるべき"

                drv.accel_dialog_type(dlg, "z")   # 全選択されているので置き換わる
                drv.accel_dialog_close(dlg, accept=True)

                after = drv.listbox_texts(page, IDC_UM_CURRENT)
                assert len(after) == 1, f"項目数が変わっている: {after}"
                assert after[0].startswith("Z"), f"アクセラレータが変更されていない: {after[0]}"
                # 機能名は変わらない（アクセラレータだけが変わる）。
                assert before[0][1:].strip() == after[0][1:].strip(), \
                    f"機能名まで変わっている: {before[0]} -> {after[0]}"
                drv.close_settings_sheet(sheet, accept=False)

    # 6. セパレータのダブルクリックは無視される（原はここでセパレータを壊していた）
    @pytest.mark.ported
    def test_ported_dblclk_on_separator_ignored(self, ported_exe_path, tmp_path):
        """移植版: セパレータをダブルクリックしてもダイアログは出ず、行も壊れない。"""
        test_file = _make_file(tmp_path, "accel_sep.dat")

        with stirling_settings(user_menus={0: [0xFFFF]}):   # セパレータ 1 件
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                sheet, page = drv.open_user_menu_page()
                before = drv.listbox_texts(page, IDC_UM_CURRENT)
                assert len(before) == 1, f"前提が崩れている: {before}"

                drv.um_dblclk_current(page, 0)
                assert drv.find_accel_dialog(timeout=1.0) is None, \
                    "セパレータでアクセラレータの指定ダイアログが出ている"
                assert drv.listbox_texts(page, IDC_UM_CURRENT) == before, \
                    "セパレータの表示が書き換えられている"
                drv.close_settings_sheet(sheet, accept=False)

    # 7. 指定したアクセラレータが設定として永続化される
    @pytest.mark.ported
    def test_ported_accel_persists_to_registry(self, ported_exe_path, tmp_path):
        """移植版: OK で確定したアクセラレータが設定へ保存される（上位16bitに格納）。"""
        test_file = _make_file(tmp_path, "accel_persist.dat")

        with stirling_settings(user_menus={0: []}):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                safe_set_focus(drv.hwnd)
                time.sleep(0.3)

                sheet, page = drv.open_user_menu_page()
                drv.um_select_available(page, 0)
                drv.um_click_add(page)

                dlg = drv.find_accel_dialog()
                assert dlg is not None
                drv.accel_dialog_type(dlg, "x")
                drv.accel_dialog_close(dlg, accept=True)

                current = drv.listbox_texts(page, IDC_UM_CURRENT)
                assert current and current[0].startswith("X"), f"割当が反映されていない: {current}"

                drv.close_settings_sheet(sheet, accept=True)   # OK で確定・永続化
                time.sleep(0.5)

            time.sleep(0.5)
            saved = read_reg_values(r"Software\StirHex\StirHex\Env")
            count = saved["UserMenu0_Count"][0]
            assert count == 1, f"メニュー1 の項目数が保存されていない: {count}"
            item = saved["UserMenu0_0"][0]
            assert (item >> 16) & 0xFF == ord("X"), \
                f"保存されたアクセラレータが 'X' でない: 0x{item:08X}"
