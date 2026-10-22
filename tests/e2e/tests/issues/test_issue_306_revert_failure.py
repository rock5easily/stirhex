"""Issue #306: 「編集前に戻す」の再読込に失敗したら文書を閉じる。

CStirlingDoc::RevertToSaved は OnOpenDocument を直接呼び、OnOpenDocument は先頭で
内容を破棄する。以前は再読込に失敗すると、空になった文書が「変更あり」・編集可のまま
残り、そのまま保存すると元ファイルが 0 バイトに置き換わった。原版は再読込に失敗すると
文書を閉じるため、移植版も同じく閉じる。

再読込は外から失敗させる。他のハンドルが共有を一切許さずにファイルを開いていると、
再読込は共有違反になり「閲覧モードで開きますか」の確認が出る。
* キャンセル: 再読込を中止する（OnOpenDocument が FALSE）
* OK: 共有を全許可で開き直すが、それも共有違反で失敗し、理由が表示される
どちらの経路でも文書は閉じられ、元ファイルは変わらないことを確認する。
"""

import time
from pathlib import Path

import pytest
import win32con
import win32file

from drivers.settings_context import stirling_settings
from drivers.stirling_driver import ID_REVERT_FILE, StirlingDriver, safe_set_focus

IDYES = 6

ORIGINAL = bytes(range(64))

VIEW_MODE_PROMPT = "閲覧モード"


def _hold_denying_all(path: Path):
    """他のプロセスが読むことも書くこともできないようにファイルを開く。"""
    return win32file.CreateFile(
        str(path),
        win32con.GENERIC_READ,
        0,
        None,
        win32con.OPEN_EXISTING,
        win32con.FILE_ATTRIBUTE_NORMAL,
        None,
    )


def _is_dirty(drv: StirlingDriver) -> bool:
    """変更ありの文書はフレームの表題が '*' で終わる（CStirlingDoc::SetModifiedFlag）。"""
    return any(title.endswith("*") for title in drv.get_mdi_child_titles())


def _wait_no_document(drv: StirlingDriver, timeout: float = 5.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not drv.get_mdi_child_titles():
            return True
        drv.wait_idle()
    return not drv.get_mdi_child_titles()


class TestRevertFailureClosesDocument:
    @pytest.mark.ported
    @pytest.mark.parametrize("answer", [win32con.IDCANCEL, win32con.IDOK],
                             ids=["view_mode_cancelled", "view_mode_reopen_fails"])
    def test_failed_revert_closes_document(self, ported_exe_path, tmp_path, answer):
        target = tmp_path / "revert_failure.dat"
        target.write_bytes(ORIGINAL)

        # 排他制御なし: アプリ自身はファイルを掴まないため、テスト側で共有を拒否できる。
        with stirling_settings(file_exclusive_mode=0, BackupCreate=0):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                safe_set_focus(drv.hwnd)
                drv.wait_idle()
                drv.type_hex_chars("10")
                assert _is_dirty(drv), "the edit must mark the document as modified"

                hold = _hold_denying_all(target)
                try:
                    drv.post_command(ID_REVERT_FILE)
                    drv.answer_message_box(IDYES)   # 編集内容を破棄して再読込する確認
                    prompt = drv.answer_message_box(answer)
                    assert VIEW_MODE_PROMPT in prompt, f"sharing violation prompt expected: {prompt!r}"
                    if answer == win32con.IDOK:
                        # 開き直しも共有違反で失敗し、読込エラーが表示される。
                        #   MB_OK のメッセージボックスの [OK] はコントロール ID が IDCANCEL(2)
                        #   のため、IDOK を送っても閉じない（実測）。
                        reason = drv.answer_message_box(win32con.IDCANCEL)
                        assert target.name in reason, f"load failure must name the file: {reason!r}"

                    # 内容を破棄した文書は残さない（残ると空のまま保存できてしまう）。
                    assert _wait_no_document(drv), "a failed revert must close the document"
                finally:
                    hold.Close()

        assert target.read_bytes() == ORIGINAL, "the file must not be touched"
