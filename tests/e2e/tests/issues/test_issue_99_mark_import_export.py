"""Issue #99: マークをファイルへ書き出し、ファイルから読み込む。

形式は Issue #96 の設定ファイルと同じ INI 風（[Mark] / [Marks]）。書式そのものの検証は
コア機能テスト（TestMarkFile*）が持つ。ここでは実際のアプリを通した往復と、読み込み時の
分岐（追加／置き換え、範囲外の読み飛ばし、不正ファイルの拒否）を確認する。
"""

from pathlib import Path

import pytest
import win32con

from drivers.stirling_driver import ID_MARK2_TOGGLE, ID_MARK_CLEAR_ALL, StirlingDriver

SAMPLE = bytes(range(256)) * 4      # 1024 バイト
MARK_A = 0x10                       # マーク1 を置く位置
MARK_B = 0x20                       # マーク2 を置く位置


def _mark_at(drv, address: int, command: int | None = None):
    """指定アドレスへ移動してマークを登録する（command 省略でマーク1）。"""
    drv.jump_to_address(f"{address:X}")
    if command is None:
        drv.mark_toggle()
    else:
        drv.post_command(command)


def _marks_in(text: str) -> dict[str, str]:
    """マークファイルの [Marks] セクションを {アドレス: 種別} に読み下す。"""
    marks = {}
    in_marks = False
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("["):
            in_marks = line.lower() == "[marks]"
            continue
        if in_marks and "=" in line and not line.startswith((";", "#")):
            key, value = line.split("=", 1)
            marks[key.strip()] = value.strip()
    return marks


@pytest.mark.ported
class TestIssue99MarkImportExport:

    def test_export_writes_addresses_and_mark_numbers(self, ported_exe_path, tmp_path):
        """書き出したファイルは、16進アドレスと 1〜3 のマーク番号で読める形になっている。"""
        source = tmp_path / "marks_src.dat"
        source.write_bytes(SAMPLE)
        out = tmp_path / "marks.mrk"

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(source)
            _mark_at(drv, MARK_A)
            _mark_at(drv, MARK_B, ID_MARK2_TOGGLE)
            drv.mark_export(out)

        text = out.read_text(encoding="utf-8")
        assert "[Mark]" in text and "Version=1" in text, f"ヘッダが無い:\n{text}"
        assert _marks_in(text) == {f"{MARK_A:X}": "1", f"{MARK_B:X}": "2"}, (
            f"マークの内容が違う:\n{text}"
        )
        assert str(source) in text, "書き出し元のパスが記録されていない"

    def test_round_trip_through_the_app(self, ported_exe_path, tmp_path):
        """書き出し → 全解除 → 読み込み → 再書き出しで、同じ内容に戻る。"""
        source = tmp_path / "marks_round.dat"
        source.write_bytes(SAMPLE)
        first = tmp_path / "first.mrk"
        second = tmp_path / "second.mrk"

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(source)
            _mark_at(drv, MARK_A)
            _mark_at(drv, MARK_B, ID_MARK2_TOGGLE)
            drv.mark_export(first)

            drv.post_command(ID_MARK_CLEAR_ALL)
            drv.mark_import(first)
            drv.answer_message_box(win32con.IDOK)   # 「n 件のマークを読み込みました」

            drv.mark_export(second)

        assert first.read_text(encoding="utf-8") == second.read_text(encoding="utf-8"), (
            "読み込み後に書き出した内容が元と一致しない"
        )

    def test_import_replaces_or_merges(self, ported_exe_path, tmp_path):
        """既存マークがあるときは追加／置き換えを問い、選んだとおりに反映する。"""
        source = tmp_path / "marks_merge.dat"
        source.write_bytes(SAMPLE)
        saved = tmp_path / "saved.mrk"
        merged = tmp_path / "merged.mrk"
        replaced = tmp_path / "replaced.mrk"

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(source)
            _mark_at(drv, MARK_A)
            drv.mark_export(saved)

            # 別の位置にマークを置いてから読み込む → 「はい」で追加
            drv.post_command(ID_MARK_CLEAR_ALL)
            _mark_at(drv, 0x30)
            drv.mark_import(saved)
            drv.answer_message_box(win32con.IDYES)   # 追加する
            drv.answer_message_box(win32con.IDOK)    # 完了通知
            drv.mark_export(merged)

            # 同じ状況で「いいえ」→ 置き換え
            drv.post_command(ID_MARK_CLEAR_ALL)
            _mark_at(drv, 0x30)
            drv.mark_import(saved)
            drv.answer_message_box(win32con.IDNO)    # 置き換える
            drv.answer_message_box(win32con.IDOK)
            drv.mark_export(replaced)

        assert set(_marks_in(merged.read_text(encoding="utf-8"))) == {f"{MARK_A:X}", "30"}, (
            "「はい」では既存マークを残したまま追加されるはず"
        )
        assert set(_marks_in(replaced.read_text(encoding="utf-8"))) == {f"{MARK_A:X}"}, (
            "「いいえ」では既存マークが置き換えられるはず"
        )

    def test_out_of_range_marks_are_skipped(self, ported_exe_path, tmp_path):
        """データ末尾を超える位置は読み飛ばし、件数を知らせる。"""
        source = tmp_path / "marks_short.dat"
        source.write_bytes(SAMPLE)
        handmade = tmp_path / "handmade.mrk"
        handmade.write_text(
            "[Mark]\nVersion=1\nSize=1024\n\n[Marks]\n10=1\nFFFFFF=3\n",
            encoding="utf-8",
        )

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(source)
            drv.mark_import(handmade)
            text = drv.answer_message_box(win32con.IDOK)

        assert "読み飛ばし" in text, f"読み飛ばしが知らされていない: {text}"

    def test_size_mismatch_asks_first(self, ported_exe_path, tmp_path):
        """別の大きさのデータで作られたファイルは、読み込む前に確認する。"""
        source = tmp_path / "marks_size.dat"
        source.write_bytes(SAMPLE)
        before = tmp_path / "size_before.mrk"
        after = tmp_path / "size_after.mrk"
        other = tmp_path / "other_size.mrk"
        other.write_text(
            "[Mark]\nVersion=1\nSize=99999\n\n[Marks]\n40=3\n", encoding="utf-8"
        )

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(source)
            _mark_at(drv, MARK_A)
            drv.mark_export(before)

            drv.mark_import(other)
            text = drv.answer_message_box(win32con.IDNO)   # 読み込まない
            assert "99999" in text, f"ファイル側の大きさが示されていない: {text}"

            drv.mark_export(after)

        assert before.read_text(encoding="utf-8") == after.read_text(encoding="utf-8"), (
            "確認を断ったのにマークが変わっている"
        )

    def test_broken_file_is_rejected_without_touching_marks(self, ported_exe_path, tmp_path):
        """解釈できないファイルは、1件も適用せずエラーにする。"""
        source = tmp_path / "marks_broken.dat"
        source.write_bytes(SAMPLE)
        before = tmp_path / "before.mrk"
        after = tmp_path / "after.mrk"
        broken = tmp_path / "broken.mrk"
        broken.write_text("[Mark]\nVersion=1\n[Marks]\n10=1\nZZZ=2\n", encoding="utf-8")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start(source)
            _mark_at(drv, MARK_A)
            drv.mark_export(before)

            drv.mark_import(broken)
            text = drv.answer_message_box(win32con.IDOK)
            assert "ZZZ" in text, f"原因が示されていない: {text}"

            drv.mark_export(after)

        assert before.read_text(encoding="utf-8") == after.read_text(encoding="utf-8"), (
            "拒否されたファイルの一部が適用されている"
        )
