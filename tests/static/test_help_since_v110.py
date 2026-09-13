"""Issue #145: v1.1.0 以降に追加・変更された機能をヘルプが説明していること。

アプリの起動は不要なドキュメント整合テスト。実装と食い違ったまま残りやすい記述
（設定の保存先、Undo のメモリ上限、マークの永続化）を固定する。
"""

import pytest

from conftest import WORKSPACE_ROOT

HELP_DIR = WORKSPACE_ROOT / "porting" / "StirHex" / "help"
SRC_DIR = HELP_DIR / "src"

# (機能, その機能を説明していると判断できる語句, 載っているべきファイル)
DOCUMENTED = [
    ("設定ファイルの保存先", "StirHex.ini", "01_intro"),
    ("/ini 起動オプション", "/ini:", "01_intro"),
    ("レジストリからの移行", "引き継ぎます", "01_intro"),
    ("マークの書き出し", "[マークの書き出し]", "03_menu"),
    ("マークの読み込み", "[マークの読み込み]", "03_menu"),
    ("マークファイルの形式", "[Marks]", "04_features"),
    ("マークの自動復元", "[マークの自動復元]", "04_features"),
    ("Undo のメモリ上限", "[メモリ上限]", "06_env"),
    ("マークの自動復元（環境設定）", "[マークの自動復元]", "06_env"),
    ("設定ファイルの所在表示", "[フォルダを開く]", "06_env"),
    ("大きいファイルの確認", "[開く前に確認する]", "06_env"),
    ("設定ファイル化（差異）", "設定ファイル", "08_differences"),
]

# 実装と食い違ったまま残ってはいけない記述。
STALE = [
    ("設定はレジストリへ保存する", "専用のレジストリ領域へ保存されます"),
    ("Undo 上限は変更できない", "この上限を環境設定画面から変更できません"),
    ("Undo 上限は非公開", "環境設定画面には公開されていません"),
    ("マークは保存されない", "マークの位置はファイルの内容や設定として保存されません"),
]


def _md(stem):
    return (SRC_DIR / (stem + ".md")).read_text(encoding="utf-8")


def _html(stem):
    return (HELP_DIR / (stem + ".html")).read_text(encoding="utf-8")


class TestIssue145HelpSinceV110:

    @pytest.mark.parametrize("what,needle,stem", DOCUMENTED)
    def test_markdown_documents_the_feature(self, what, needle, stem):
        assert needle in _md(stem), "%s.md が %s を説明していない" % (stem, what)

    @pytest.mark.parametrize("what,needle,stem", DOCUMENTED)
    def test_generated_html_is_in_sync(self, what, needle, stem):
        # 生成 HTML は `[` をそのまま出す（記法ではないため）。角括弧付きの
        # UI 名も含めて、ソースと同じ語句が出ていることを確認する。
        assert needle in _html(stem), "%s.html が %s を説明していない" % (stem, what)

    @pytest.mark.parametrize("what,needle", STALE)
    def test_stale_statements_are_gone(self, what, needle):
        hits = [p.name for p in sorted(SRC_DIR.glob("*.md")) + sorted(HELP_DIR.glob("*.html"))
                if needle in p.read_text(encoding="utf-8")]
        assert not hits, "実装と食い違う記述（%s）が残っている: %s" % (what, hits)
