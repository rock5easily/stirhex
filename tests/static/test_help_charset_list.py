"""Issue #127: ヘルプのキャラクターセット一覧へ UTF-8 を反映する。

UTF-8 対応後もヘルプの一部の一覧が従来の6種類のままで、同じヘルプ内の説明
（08_differences）と矛盾していた。Markdown ソースと生成 HTML の双方で、
選択可能な一覧に UTF-8 が含まれることを固定する。

アプリの起動は不要なドキュメント整合テスト。
"""

import re

import pytest

from conftest import WORKSPACE_ROOT

HELP_DIR = WORKSPACE_ROOT / "porting" / "StirHex" / "help"
SRC_DIR = HELP_DIR / "src"

# 選択可能なキャラクターセット（メニューと同じ並び）。
CHARSETS = ["ASCII", "SHIFT-JIS", "EUC", "Unicode", "UTF-8", "EBCDIC", "EBCIDK"]

# 一覧が載っている箇所（ファイル, 一覧を含む行の目印）。
ENUMERATION_SITES = [
    ("02_basics", "選択できる項目は"),      # 設定→キャラクターセット
    ("04_features", "文字列用のキャラクターセット"),  # BGREP の文字列検索
]


def _text(path):
    return path.read_text(encoding="utf-8")


def _enumeration_block(text: str, marker: str) -> str:
    """目印を含む行から、一覧が閉じる `EBCIDK` までをひとまとまりとして返す。

    Markdown 側の一覧は行をまたぐことがあるため、行単位では判定しない。
    """
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if marker in line:
            block = "\n".join(lines[i:i + 4])
            assert "EBCIDK" in block, "一覧の終端が見つからない: %s" % block
            return block[:block.index("EBCIDK") + len("EBCIDK")]
    raise AssertionError("一覧の目印が見つからない: %s" % marker)


def _enumeration_lines(text: str) -> list[str]:
    """EBCDIC と EBCIDK を同時に含む行＝キャラクターセットの列挙とみなす。"""
    return [line for line in text.splitlines()
            if "EBCDIC" in line and "EBCIDK" in line]


class TestIssue127HelpCharsetList:

    @pytest.mark.parametrize("stem,marker", ENUMERATION_SITES)
    def test_markdown_lists_utf8(self, stem, marker):
        block = _enumeration_block(_text(SRC_DIR / (stem + ".md")), marker)
        for name in CHARSETS:
            assert name in block, "%s.md の一覧に %s が無い: %s" % (stem, name, block)

    @pytest.mark.parametrize("stem,marker", ENUMERATION_SITES)
    def test_generated_html_matches_source(self, stem, marker):
        block = _enumeration_block(_text(HELP_DIR / (stem + ".html")), marker)
        for name in CHARSETS:
            assert name in block, "%s.html の一覧に %s が無い: %s" % (stem, name, block)

    def test_no_charset_enumeration_omits_utf8(self):
        """ヘルプ全体で、6種類のままの列挙が残っていないこと（更新漏れ検出）。"""
        stale = []
        for path in sorted(SRC_DIR.glob("*.md")) + sorted(HELP_DIR.glob("*.html")):
            for line in _enumeration_lines(_text(path)):
                if "UTF-8" not in line:
                    stale.append("%s: %s" % (path.name, line.strip()[:120]))
        assert not stale, "UTF-8 を欠く列挙:\n" + "\n".join(stale)

    def test_enumeration_order_places_utf8_after_unicode(self):
        """一覧の並びはメニューと同じ（Unicode の直後に UTF-8）。"""
        for stem, _marker in ENUMERATION_SITES:
            for path in (SRC_DIR / (stem + ".md"), HELP_DIR / (stem + ".html")):
                text = _text(path)
                for line in _enumeration_lines(text):
                    positions = [line.find(name) for name in CHARSETS]
                    if any(p < 0 for p in positions):
                        continue   # 行をまたぐ一覧はここでは見ない
                    assert positions == sorted(positions), (
                        "%s の並びがメニューと異なる: %s" % (path.name, line.strip()[:120])
                    )
                    # 行をまたぐ Markdown も含め、Unicode→UTF-8 の順は本文全体で確認する。
                assert re.search(r"Unicode.{0,20}UTF-8", text, re.S), (
                    "%s: Unicode の直後に UTF-8 が無い" % path.name
                )
