"""Issue #100: マークの自動保存／自動復元。

設定 markAutoRestore が ON の間だけ、文書を閉じるときにマークを設定ファイルへ記録し、
同じファイルを開いたときに復元する。OFF の間は読みも書きもしない — 復元しない状態で
書き戻すと、利用者が見ていないマークを 0 件で上書きしてしまうため。

1行表現そのものの検証はコア機能テスト（TestMarkList*）が持つ。ここではアプリを通した
保存と復元、および OFF・サイズ変化の各分岐を確認する。
"""

import contextlib

import pytest
import win32con

from drivers.settings_context import read_reg_values, registry_section, settings_value
from drivers.stirling_driver import ID_MARK2_TOGGLE, StirlingDriver

PORT_ENV = r"Software\StirHex\StirHex\Env"
PORT_MARK_STORE = r"Software\StirHex\StirHex\MarkStore"

SAMPLE = bytes(range(256)) * 4      # 1024 バイト
MARK_A = 0x10
MARK_B = 0x20


@contextlib.contextmanager
def _auto_restore(enabled: bool):
    with settings_value(PORT_ENV, "MarkAutoRestore", 1 if enabled else 0):
        yield


@pytest.fixture(autouse=True)
def _clean_mark_store():
    """マークストアを空にしてから始め、テスト後に元へ戻す。

    このストアはパスをキーに持ち越されるため、前のテストの記録が残っていると
    「書かれていないこと」の判定が汚れる。
    """
    with registry_section(PORT_MARK_STORE):
        yield


def _mark_addresses(drv) -> list[int]:
    """マーク一覧ダイアログに並ぶアドレスを読む（マークが無くてもダイアログは開く）。

    一覧の item_data は行番号なので、表示文字列の16進アドレスを読む。
    """
    dialog = drv.open_mark_list_dialog()
    try:
        rows = drv.mark_list_entries(dialog)
    finally:
        drv.click_dialog_button(dialog, win32con.IDCANCEL)
    return sorted(int(text.split()[0], 16) for text, _data in rows)


def _put_marks(drv, path):
    """ファイルを開いて2箇所にマークを付け、閉じる（アプリも終了する）。"""
    drv.start(path)
    drv.jump_to_address(f"{MARK_A:X}")
    drv.mark_toggle()
    drv.jump_to_address(f"{MARK_B:X}")
    drv.post_command(ID_MARK2_TOGGLE)


@pytest.mark.ported
class TestIssue100MarkAutoRestore:

    def test_marks_are_restored_on_reopen(self, ported_exe_path, tmp_path):
        """設定 ON なら、閉じたときのマークが次に開いたときに戻る。"""
        target = tmp_path / "auto_restore.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                _put_marks(drv, target)

            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                addresses = _mark_addresses(drv)

        assert addresses == [MARK_A, MARK_B], f"マークが復元されていない: {addresses}"

    def test_disabled_setting_records_nothing(self, ported_exe_path, tmp_path):
        """設定 OFF の間は、マークを付けて閉じても設定ファイルに書かない。"""
        target = tmp_path / "no_record.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(False):
            with StirlingDriver(ported_exe_path) as drv:
                _put_marks(drv, target)

            assert not read_reg_values(PORT_MARK_STORE), (
                "設定 OFF なのにマークストアが書かれている"
            )

    def test_disabled_setting_keeps_a_previous_record(self, ported_exe_path, tmp_path):
        """ON で記録した後に OFF にしても、開いて閉じただけで記録は消えない。

        OFF の間は復元しないため、閉じるときのマーク 0 件は「利用者が消した」ではなく
        「復元しなかった」結果でしかない。それで上書きすると気付けないまま記録を失う。
        """
        target = tmp_path / "keep_record.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                _put_marks(drv, target)
            recorded = read_reg_values(PORT_MARK_STORE)

        assert recorded, "ON で記録されていない（前提が崩れている）"

        with _auto_restore(False):
            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert not _mark_addresses(drv), "設定 OFF なのに復元されている"

            assert read_reg_values(PORT_MARK_STORE) == recorded, (
                "設定 OFF で開いて閉じただけで、以前の記録が変わっている"
            )

    def test_marks_cleared_by_the_user_are_forgotten(self, ported_exe_path, tmp_path):
        """設定 ON で全て解除して閉じれば、記録も消える（利用者の明示操作のため）。"""
        target = tmp_path / "cleared.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                _put_marks(drv, target)

            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert _mark_addresses(drv), "復元されていない（前提が崩れている）"
                drv.post_command(32845)   # ID_MARK_CLEAR_ALL

            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert not _mark_addresses(drv), "解除したマークが復活している"

    def test_size_change_skips_restore(self, ported_exe_path, tmp_path):
        """記録時と大きさが違うファイルには復元しない（位置がずれているため）。"""
        target = tmp_path / "resized.dat"
        target.write_bytes(SAMPLE)

        with _auto_restore(True):
            with StirlingDriver(ported_exe_path) as drv:
                _put_marks(drv, target)

            target.write_bytes(SAMPLE + b"\xFF" * 16)   # 外部で大きさが変わった

            with StirlingDriver(ported_exe_path) as drv:
                drv.start(target)
                assert not _mark_addresses(drv), "大きさが変わっているのに復元されている"
