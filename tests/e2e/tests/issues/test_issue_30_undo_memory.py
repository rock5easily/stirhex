"""Issue #30: Undo レコードのメモリ保持量の上限（移植版のみ）。

移植版は Undo/Redo が保持する退避データの合計に上限（レジストリ Env の値 UndoMemoryLimitMB）を
設ける。退避量が上限を超える範囲操作は事前に確認ダイアログを出し、

- 中止 → ドキュメントを一切変更しない
- 続行 → 退避せずに実行し、Undo 履歴を破棄する（この操作は取り消せない）

原版には無い移植独自の保護のため、ゴールデン比較は行わない。
"""

import time
import pytest

from drivers.stirling_driver import (
    StirlingDriver,
    CMD_EDIT_UNDO,
    ID_DELETE_SELECTION,
)
from drivers.settings_context import stirling_settings

IDYES = 6
IDNO = 7

# 上限 1MB に対して 2MB のファイルを全選択削除する（退避量 2MB > 上限 1MB）。
UNDO_LIMIT_MB = 1
TEST_DATA = bytes(range(256)) * (8 * 1024)   # 2 MiB


def _write_test_file(path):
    path.write_bytes(TEST_DATA)
    return path


@pytest.mark.ported
class TestIssue30UndoMemoryLimit:

    def test_ported_cancel_keeps_document_intact(self, ported_exe_path, tmp_path):
        """確認ダイアログで中止したら、データも選択も変化しないこと。"""
        test_file = _write_test_file(tmp_path / "undo_limit_cancel.dat")
        out_file = tmp_path / "undo_limit_cancel_out.dat"

        with stirling_settings(UndoMemoryLimitMB=UNDO_LIMIT_MB):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()

                drv.select_all()
                time.sleep(0.3)
                # 確認ダイアログはモーダルのため、プロセス跨ぎの SendMessage（press_delete）では
                # デッドロックする。PostMessage 経由のコマンドで削除させる。
                drv.post_command(ID_DELETE_SELECTION)

                text = drv.answer_message_box(IDNO, timeout=15.0)
                assert "続行" in text, f"Undo capacity confirmation not shown: {text!r}"

                time.sleep(0.5)
                drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        assert out_file.read_bytes() == TEST_DATA, "Cancelled delete must not modify the document"

    def test_ported_continue_discards_undo_history(self, ported_exe_path, tmp_path):
        """確認ダイアログで続行したら削除され、Undo しても復元されないこと。"""
        test_file = _write_test_file(tmp_path / "undo_limit_continue.dat")
        out_file = tmp_path / "undo_limit_continue_out.dat"

        with stirling_settings(UndoMemoryLimitMB=UNDO_LIMIT_MB):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(test_file)
                drv.focus_view()

                drv.select_all()
                time.sleep(0.3)
                # 確認ダイアログはモーダルのため、プロセス跨ぎの SendMessage（press_delete）では
                # デッドロックする。PostMessage 経由のコマンドで削除させる。
                drv.post_command(ID_DELETE_SELECTION)

                text = drv.answer_message_box(IDYES, timeout=15.0)
                assert "続行" in text, f"Undo capacity confirmation not shown: {text!r}"

                # 2MB の削除は 1 バイトずつ処理されるため完了まで待つ（Issue #62）。
                time.sleep(20.0)

                drv.post_command(CMD_EDIT_UNDO)   # 履歴は破棄済み＝何も起きない
                time.sleep(1.0)

                drv.save_as_via_dialog(out_file)

        assert out_file.exists()
        assert out_file.read_bytes() == b"", "Undo-less delete must empty the document and stay undone-proof"
