"""Issue #304: 同梱ヘルプの HTML が Markdown 原稿から再生成した結果と一致すること。

HTML だけを直接書き換えると、次にヘルプを再生成したときにその変更が消える。
原稿（help/src/*.md）を一時ディレクトリへビルドし、コミット済みの HTML と比べる。
"""

import sys

from conftest import WORKSPACE_ROOT

TOOLS = WORKSPACE_ROOT / "porting/tools"
sys.path.insert(0, str(TOOLS))
import build_help

HELP_DIR = WORKSPACE_ROOT / "porting" / "StirHex" / "help"


def read_normalized(path):
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def test_committed_help_html_matches_markdown_sources(tmp_path):
    assert build_help.build(tmp_path) == 0
    generated = sorted(p.name for p in tmp_path.glob("*.html"))
    committed = sorted(p.name for p in HELP_DIR.glob("*.html"))
    assert generated == committed, "生成されるページとコミット済みのページが一致しない"

    stale = [name for name in generated
             if read_normalized(tmp_path / name) != read_normalized(HELP_DIR / name)]
    assert not stale, (
        f"原稿から再生成した HTML と一致しない: {stale}。"
        "HTML を直接編集せず help/src/*.md を直して porting/tools/build_help.py で再生成すること"
    )
