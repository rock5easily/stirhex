"""Issue #102: Undo/Redo のメモリ上限を環境設定から変更できる。

上限そのものは Issue #30 からあり、破棄計画のロジックはコア機能テスト（PlanUndoTrim /
TestUndoBudget）で網羅済み。ここでは UI と設定の配線、および上限が実際に古い Undo
レコードを落とすことを確認する。
"""

import contextlib

import pytest
import win32con
import win32gui

from drivers.settings_context import read_reg_values, settings_value
from drivers.stirling_driver import (
    IDC_ED1_UNDO_LIMIT,
    IDC_ED1_UNDO_MB,
    IDC_ED1_UNDO_MB_SPIN,
    IDC_ED1_UNDO_MB_UNIT,
    StirlingDriver,
    _set_control_text,
)

PORT_ENV = r"Software\StirHex\StirHex\Env"

MB = 1024 * 1024
# 1MB の Fill を 2 回行う。1 レコードが上限ちょうどになるため、2 回目で最古のレコードが
# 落ちる。1 バイト編集を積み上げる方式では 100 万回の操作が必要になる。
FILL_A = 0xAA
FILL_B = 0xBB


@contextlib.contextmanager
def _undo_limit(enabled: bool, limit_mb: int):
    """アンドゥバッファのメモリ上限を一時的に差し替える。"""
    with settings_value(PORT_ENV, "UndoMemoryLimit", 1 if enabled else 0):
        with settings_value(PORT_ENV, "UndoMemoryLimitMB", limit_mb):
            yield


def _two_fills_then_two_undos(drv, source, out_path):
    """1MB ずつ 2 箇所を Fill し、Undo を 2 回行った結果を保存して返す。"""
    drv.start(source)
    drv.select_range_dialog("0", f"{MB - 1:X}")
    drv.fill_range_dialog(f"{FILL_A:02X}")
    drv.select_range_dialog(f"{MB:X}", f"{2 * MB - 1:X}")
    drv.fill_range_dialog(f"{FILL_B:02X}")
    drv.undo()
    drv.undo()
    drv.save_as_via_dialog(out_path)
    return out_path.read_bytes()


@pytest.mark.ported
class TestIssue102UndoMemory:

    def test_limit_drops_the_oldest_undo_record(self, ported_exe_path, tmp_path):
        """上限に収まらない古い Undo レコードは破棄され、その編集は元に戻せない。"""
        source = tmp_path / "undo_small_limit.dat"
        source.write_bytes(b"\x00" * (2 * MB))
        out = tmp_path / "out_small_limit.dat"

        with _undo_limit(True, 1):
            with StirlingDriver(ported_exe_path) as drv:
                data = _two_fills_then_two_undos(drv, source, out)

        assert len(data) == 2 * MB, f"サイズが変わっている: {len(data)}"
        assert data[0] == FILL_A, (
            "上限を超えて破棄されたはずの 1 回目の Fill が元に戻っている"
        )
        assert data[MB] == 0x00, "2 回目の Fill が元に戻っていない"

    def test_generous_limit_keeps_both_records(self, ported_exe_path, tmp_path):
        """上限に収まっていれば、同じ操作で両方の編集を元に戻せる。"""
        source = tmp_path / "undo_large_limit.dat"
        source.write_bytes(b"\x00" * (2 * MB))
        out = tmp_path / "out_large_limit.dat"

        with _undo_limit(True, 256):
            with StirlingDriver(ported_exe_path) as drv:
                data = _two_fills_then_two_undos(drv, source, out)

        assert data[0] == 0x00, "1 回目の Fill が元に戻っていない"
        assert data[MB] == 0x00, "2 回目の Fill が元に戻っていない"

    def test_lowering_the_limit_trims_open_documents_at_once(self, ported_exe_path, tmp_path):
        """上限を下げた時点で、開いている文書の古い Undo レコードが破棄される。

        次の編集まで反映を遅らせない（Issue #102 の設計判断）ことの確認。編集を続けなくても
        超過分を抱え込まないという意味であり、UI 上は Undo できる回数が即座に減って見える。
        """
        source = tmp_path / "undo_lowered.dat"
        source.write_bytes(b"\x00" * (2 * MB))
        out = tmp_path / "out_lowered.dat"

        with _undo_limit(True, 256):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(source)
                drv.select_range_dialog("0", f"{MB - 1:X}")
                drv.fill_range_dialog(f"{FILL_A:02X}")
                drv.select_range_dialog(f"{MB:X}", f"{2 * MB - 1:X}")
                drv.fill_range_dialog(f"{FILL_B:02X}")

                # ここまでは上限 256MB なので 2 件とも保持されている。
                sheet, page = drv.open_edit1_page()
                _set_control_text(win32gui.GetDlgItem(page, IDC_ED1_UNDO_MB), "1")
                drv.close_settings_sheet(sheet, accept=True)

                drv.undo()
                drv.undo()
                drv.save_as_via_dialog(out)
                data = out.read_bytes()

        assert data[0] == FILL_A, (
            "上限を下げた時点で破棄されるはずの 1 回目の Fill が元に戻っている"
        )
        assert data[MB] == 0x00, "2 回目の Fill が元に戻っていない"

    def test_checkbox_disables_the_input(self, ported_exe_path):
        """上限のチェックを外すと、入力欄・スピン・単位ラベルが無効化される。"""
        with _undo_limit(True, 256):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                sheet, page = drv.open_edit1_page()
                try:
                    dependents = (IDC_ED1_UNDO_MB, IDC_ED1_UNDO_MB_SPIN, IDC_ED1_UNDO_MB_UNIT)
                    assert all(win32gui.IsWindowEnabled(win32gui.GetDlgItem(page, i))
                               for i in dependents), "チェック ON で有効になっていない"

                    # BM_CLICK はチェック状態を反転し、親へ BN_CLICKED も送る（二重に
                    #   WM_COMMAND を送ると 2 回トグルしてしまう）。
                    check = win32gui.GetDlgItem(page, IDC_ED1_UNDO_LIMIT)
                    win32gui.SendMessage(check, win32con.BM_CLICK, 0, 0)

                    assert not any(win32gui.IsWindowEnabled(win32gui.GetDlgItem(page, i))
                                   for i in dependents), "チェック OFF で無効化されない"
                finally:
                    drv.close_settings_sheet(sheet, accept=False)

    def test_setting_round_trips_through_the_dialog(self, ported_exe_path):
        """環境設定で変えた値が設定ファイルへ保存される。"""
        with _undo_limit(True, 256):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start()
                sheet, page = drv.open_edit1_page()
                edit = win32gui.GetDlgItem(page, IDC_ED1_UNDO_MB)
                assert edit, "上限の入力欄が見つからない"
                _set_control_text(edit, "8")
                drv.close_settings_sheet(sheet, accept=True)

            saved = read_reg_values(PORT_ENV)
            assert saved["UndoMemoryLimitMB"][0] == 8, (
                f"変更した上限が保存されていない: {saved.get('UndoMemoryLimitMB')}"
            )
