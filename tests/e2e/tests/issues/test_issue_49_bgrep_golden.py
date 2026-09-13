"""BGREP（フォルダ横断検索）の原版突き合わせとオプション検証（Issue #49 / 親 #16、#205）。

BGREP はダイアログ入力（検索データ・ファイル種別・フォルダ）と結果のアウトプット
ペインという、Unicode 化（#41）とシェル系モダナイズ（#45）の両方が通る経路。
原版と移植版へ同じ条件を与え、アウトプットペインの結果行が一致することを確認する。

原・移植ともアウトプットペインは素の CListBox で、1 行は "フルパス : %08X"。
ダイアログ操作と結果読み取りのヘルパは drivers/bgrep.py にある（4GiB のケースからも
使うため。#205）。
"""
import time
from pathlib import Path

import pytest

from drivers import bgrep
from drivers.stirling_driver import StirlingDriver

PATTERN = b"\xDE\xAD\xC0\xDE"
# 原版の 16 進入力はバイトを空白で区切る（既定値 "AA BB CC" と同じ書式）。
PATTERN_HEX = "DE AD C0 DE"


def _make_corpus(root: Path) -> None:
    """検索対象フォルダを作る。ヒット 3 件（2 ファイル）＋非ヒット 1 件。"""
    filler = bytes(range(256))
    # hit_a.dat: 0x00000010 と 0x00000100 の 2 箇所
    a = bytearray(filler * 2)
    a[0x10:0x14] = PATTERN
    a[0x100:0x104] = PATTERN
    (root / "hit_a.dat").write_bytes(bytes(a))
    # hit_b.dat: 0x00000004 の 1 箇所
    b = bytearray(filler)
    b[0x04:0x08] = PATTERN
    (root / "hit_b.dat").write_bytes(bytes(b))
    # miss.dat: ヒットなし
    (root / "miss.dat").write_bytes(filler)
    # 拡張子が対象外のファイル（*.dat 指定で拾われないこと）
    other = bytearray(filler)
    other[0x20:0x24] = PATTERN
    (root / "other.bin").write_bytes(bytes(other))


class TestIssue49BgrepGolden:
    """BGREP の検出結果が原版と一致することを確認する。"""

    @pytest.mark.golden
    def test_bgrep_hits_match_original(self, original_exe_path, ported_exe_path, tmp_path):
        orig_dir = tmp_path / "orig"
        port_dir = tmp_path / "port"
        orig_dir.mkdir()
        port_dir.mkdir()
        _make_corpus(orig_dir)
        _make_corpus(port_dir)

        with StirlingDriver(original_exe_path) as drv:
            drv.start()
            time.sleep(0.5)
            orig_lines, orig_applied = bgrep.run(drv, orig_dir, PATTERN_HEX)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            time.sleep(0.5)
            port_lines, port_applied = bgrep.run(drv, port_dir, PATTERN_HEX)

        orig_norm = bgrep.normalize(orig_lines, orig_dir)
        port_norm = bgrep.normalize(port_lines, port_dir)

        assert orig_norm, f"原版が結果を返していない: {orig_lines}"
        assert port_norm == orig_norm, (
            "BGREP の結果が原版と一致しない\n"
            f"原版 : {orig_norm}\n  実効値: {orig_applied}\n"
            f"移植版: {port_norm}\n  実効値: {port_applied}"
        )


class TestIssue49BgrepOptions:
    """再帰・拡張子・文字列検索・結果からのオープン（移植版。Issue #205）。

    上のケースは 16進・再帰なし・`*.dat` に固定で、ダイアログの他の条件も、結果一覧を
    使う操作も通っていなかった。
    """

    @pytest.mark.ported
    def test_recurse_option_controls_subfolder_search(self, ported_exe_path, tmp_path):
        """サブフォルダのヒットは、再帰オンのときだけ結果に出る。"""
        root = tmp_path / "corpus"
        root.mkdir()
        _make_corpus(root)
        deep = root / "sub" / "deeper"
        deep.mkdir(parents=True)
        nested = bytearray(bytes(range(256)))
        nested[0x30:0x34] = PATTERN
        (deep / "nested.dat").write_bytes(bytes(nested))

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            time.sleep(0.5)

            off_lines, off_applied = bgrep.run(drv, root, PATTERN_HEX, recurse=False)
            off = bgrep.normalize(off_lines, root)
            assert off, f"再帰オフでも直下のヒットは出る: {off_lines} / {off_applied}"
            assert not any("nested.dat" in line for line in off), (
                f"再帰オフでサブフォルダを検索している: {off}"
            )

            on_lines, on_applied = bgrep.run(drv, root, PATTERN_HEX, recurse=True)
            on = bgrep.normalize(on_lines, root)
            assert any("nested.dat" in line for line in on), (
                f"再帰オンでサブフォルダのヒットが出ない: {on} / {on_applied}"
            )
            assert len(on) > len(off), "再帰オンは直下のヒットも含む"

    @pytest.mark.ported
    def test_file_mask_selects_the_files_to_scan(self, ported_exe_path, tmp_path):
        """対象拡張子の指定で走査対象が変わる（*.dat では other.bin を拾わない）。"""
        root = tmp_path / "corpus_mask"
        root.mkdir()
        _make_corpus(root)   # other.bin にも PATTERN がある

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            time.sleep(0.5)

            dat_only = bgrep.normalize(bgrep.run(drv, root, PATTERN_HEX, file_mask="*.dat")[0], root)
            assert dat_only, "*.dat のヒットが無い"
            assert not any("other.bin" in line for line in dat_only), (
                f"対象外の拡張子まで走査している: {dat_only}"
            )

            bin_only = bgrep.normalize(bgrep.run(drv, root, PATTERN_HEX, file_mask="*.bin")[0], root)
            assert any("other.bin" in line for line in bin_only), (
                f"*.bin を指定しても other.bin のヒットが出ない: {bin_only}"
            )
            assert not any("hit_a.dat" in line for line in bin_only), (
                f"*.bin の指定で .dat まで走査している: {bin_only}"
            )

    @pytest.mark.ported
    def test_text_search_finds_japanese_string(self, ported_exe_path, tmp_path):
        """文字列検索（CP932 の日本語）でヒット位置が返る。"""
        root = tmp_path / "corpus_text"
        root.mkdir()
        needle = "検索対象"
        body = b"HEAD" + needle.encode("cp932") + b"TAIL"
        (root / "text_hit.dat").write_bytes(body)
        (root / "text_miss.dat").write_bytes(b"NOTHING TO SEE HERE")

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            time.sleep(0.5)
            lines, applied = bgrep.run(drv, root, needle, hex_mode=False)

        found = bgrep.normalize(lines, root)
        assert any("text_hit.dat" in line for line in found), (
            f"文字列検索でヒットしない: {found} / 実効値: {applied}"
        )
        assert not any("text_miss.dat" in line for line in found), (
            f"含まれないファイルまでヒットしている: {found}"
        )
        # 行は "フルパス : %08X"。ヒット位置は "HEAD" の直後。
        hit = [line for line in found if "text_hit.dat" in line][0]
        assert hit.endswith(f"{len(b'HEAD'):08x}"), f"ヒット位置が違う: {hit}"

    @pytest.mark.ported
    def test_result_line_opens_the_file_at_the_hit(self, ported_exe_path, tmp_path):
        """結果行を実行すると、そのファイルがヒット位置で開く。"""
        root = tmp_path / "corpus_open"
        root.mkdir()
        _make_corpus(root)

        with StirlingDriver(ported_exe_path) as drv:
            drv.start()
            time.sleep(0.5)
            lines, _applied = bgrep.run(drv, root, PATTERN_HEX)
            assert lines, "結果が無いと開く操作を確認できない"

            # hit_b.dat（0x00000004 の 1 件）の行を選んで実行する。
            index = next(i for i, line in enumerate(lines) if "hit_b.dat" in line.lower())
            bgrep.activate_output_line(drv, index)
            time.sleep(1.0)

            titles = drv.get_mdi_child_titles()
            assert any("hit_b.dat" in title for title in titles), (
                f"結果から対象ファイルが開かれない: {titles}"
            )
            assert drv.get_statusbar_pane_text(1) == "0x00000004", (
                f"ヒット位置へ移動していない: {drv.get_all_statusbar_text()}"
            )
