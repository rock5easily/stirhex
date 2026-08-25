#!/usr/bin/env python3
"""StirHex help builder: Markdown (subset) -> static HTML pages.

Input : porting/StirHex/help/src/*.md   (NN_slug.md, ordered by NN)
Output: porting/StirHex/help/*.html     (index.html + one page per chapter)

The Markdown subset intentionally stays small so no third-party package is
needed at build time:

    # .. ###### headings        **bold**  *italic*  `code`
    paragraphs                  [text](url)
    - / * bullet lists          1. ordered lists     (nested by 2 spaces)
    | GFM | tables |            > blockquote
    fenced code blocks          --- horizontal rule

Anything else is emitted as plain text, so unsupported syntax stays visible
rather than being silently dropped.
"""
from __future__ import annotations

import html
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

HELP_DIR = Path(__file__).resolve().parent.parent / "StirHex" / "help"
SRC_DIR = HELP_DIR / "src"

CSS = """
:root { color-scheme: light; }
body { max-width: 56rem; margin: 0 auto; padding: 1.5rem 1.25rem 4rem; color: #182433;
       background: #fff; font-family: "Yu Gothic UI", "Meiryo", sans-serif; line-height: 1.75; }
h1, h2, h3, h4 { color: #073e69; line-height: 1.4; }
h1 { border-bottom: 2px solid #19bad2; padding-bottom: .3rem; }
h2 { margin-top: 2.4rem; border-bottom: 1px solid #cfe2ee; padding-bottom: .2rem; }
h3 { margin-top: 1.8rem; }
code { background: #edf4f8; padding: .1rem .3rem; font-family: "Consolas", monospace; }
pre { background: #edf4f8; padding: .8rem 1rem; overflow-x: auto; }
pre code { background: none; padding: 0; }
table { border-collapse: collapse; margin: 1rem 0; }
th, td { border: 1px solid #cfe2ee; padding: .35rem .7rem; text-align: left; vertical-align: top; }
th { background: #f0fbfd; }
blockquote { border-left: .3rem solid #19bad2; margin: 1rem 0; padding: .5rem 1rem;
             background: #f0fbfd; }
hr { border: none; border-top: 1px solid #cfe2ee; margin: 2rem 0; }
a { color: #0b6ba8; }
nav.toc { background: #f7fafc; border: 1px solid #e0eaf1; padding: .8rem 1.2rem; margin: 1.5rem 0; }
nav.toc ul { margin: .3rem 0; padding-left: 1.2rem; }
nav.chapters { margin: 2.5rem 0 0; padding-top: 1rem; border-top: 1px solid #cfe2ee;
               display: flex; justify-content: space-between; gap: 1rem; font-size: .95rem; }
.home { font-size: .95rem; margin-bottom: 1rem; }
.wrap { overflow-x: auto; }
"""

PAGE = """<!doctype html>
<html lang="ja">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>{css}</style>
</head>
<body>
{home}{body}{nav}
</body>
</html>
"""


@dataclass
class Heading:
    level: int
    text: str
    anchor: str


@dataclass
class Chapter:
    path: Path
    title: str = ""
    body: str = ""
    headings: list[Heading] = field(default_factory=list)

    @property
    def out_name(self) -> str:
        return self.path.stem + ".html"


def slugify(text: str, used: dict[str, int]) -> str:
    """Anchor from a heading. Uniqueness is checked on the emitted value, so a
    document holding both "foo" and "foo-2" cannot collide on "foo-2"."""
    base = re.sub(r"[^0-9A-Za-z々ー〆ぁ-ヿ一-鿿豈-﫿]+", "-", text).strip("-").lower()
    base = base or "section"
    anchor = base
    while anchor in used:
        used[base] = used.get(base, 1) + 1
        anchor = f"{base}-{used[base]}"
    used[anchor] = 1
    used.setdefault(base, 1)
    return anchor


def inline(text: str) -> str:
    """Escape, then re-introduce the handful of inline constructs we support."""
    placeholders: list[str] = []

    def stash(m: re.Match[str]) -> str:
        placeholders.append("<code>" + html.escape(m.group(1)) + "</code>")
        return f"\x00{len(placeholders) - 1}\x00"

    text = re.sub(r"`([^`]+)`", stash, text)
    text = html.escape(text)
    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", r'<a href="\2">\1</a>', text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    # Non-space at both inner edges, so "2 * x * y" and "*.*" stay literal.
    text = re.sub(r"(?<![*\w])\*(\S|\S[^*]*\S)\*(?!\*)", r"<em>\1</em>", text)
    return re.sub(r"\x00(\d+)\x00", lambda m: placeholders[int(m.group(1))], text)


def split_row(line: str) -> list[str]:
    r"""Split one table row on the cell separators only: a pipe inside `code`
    or written as \| belongs to the cell, and empty edge cells survive."""
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]

    cells: list[str] = []
    buf: list[str] = []
    in_code = False
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == "\\" and i + 1 < len(line) and line[i + 1] == "|":
            buf.append("|")
            i += 2
            continue
        if ch == "`":
            in_code = not in_code
        if ch == "|" and not in_code:
            cells.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
        i += 1
    cells.append("".join(buf).strip())
    return cells


def is_block_start(stripped: str) -> bool:
    return bool(
        stripped.startswith(("#", "```", ">", "|"))
        or re.match(r"[-*]\s+", stripped)
        or re.match(r"\d+[.)]\s+", stripped)
        or re.fullmatch(r"(-{3,}|\*{3,}|_{3,})", stripped)
    )


def indent_of(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


CJK = re.compile(r"[぀-ヿ㐀-鿿豈-﫿、-〿！-｠]")


def join_wrapped(parts: list[str]) -> str:
    """Join soft-wrapped source lines. A break between two CJK characters is a
    typesetting artefact, so it joins with nothing; elsewhere a space is kept."""
    text = parts[0] if parts else ""
    for part in parts[1:]:
        if not part:
            continue
        glue = "" if text and CJK.match(text[-1]) and CJK.match(part[0]) else " "
        text += glue + part
    return text


def render_list(lines: list[str], i: int, chapter: Chapter, used: dict[str, int]) -> tuple[int, str]:
    base = indent_of(lines[i])
    ordered = bool(re.match(r"\d+[.)]\s+", lines[i].strip()))
    items: list[list[str]] = []
    tails: list[str] = []
    while i < len(lines) and lines[i].strip():
        cur = indent_of(lines[i])
        stripped = lines[i].strip()
        m = re.match(r"(?:[-*]|\d+[.)])\s+(.*)", stripped)
        if cur < base or (not m and cur <= base):
            break
        if not m and items:
            # Indented continuation of the item above (soft-wrapped text).
            items[-1].append(stripped)
            i += 1
            continue
        if cur > base and items:
            i, nested = render_list(lines, i, chapter, used)
            tails[-1] += nested
            continue
        items.append([m.group(1)])
        tails.append("")
        i += 1
    tag = "ol" if ordered else "ul"
    body = "".join(
        f"<li>{inline(join_wrapped(item))}{tail}</li>" for item, tail in zip(items, tails)
    )
    return i, f"<{tag}>{body}</{tag}>"


def render(lines: list[str], chapter: Chapter, used: dict[str, int], collect: bool = True) -> str:
    out: list[str] = []
    i = 0
    while i < len(lines):
        stripped = lines[i].strip()

        if not stripped:
            i += 1
            continue

        if stripped.startswith("```"):
            lang = stripped[3:].strip()
            i += 1
            block: list[str] = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                block.append(lines[i])
                i += 1
            i += 1
            cls = f' class="language-{html.escape(lang)}"' if lang else ""
            out.append(f"<pre><code{cls}>" + html.escape("\n".join(block)) + "</code></pre>")
            continue

        m = re.match(r"(#{1,6})\s+(.*)", stripped)
        if m:
            level, text = len(m.group(1)), m.group(2).strip()
            anchor = slugify(text, used)
            # Headings quoted inside a blockquote are not part of the outline.
            if collect:
                chapter.headings.append(Heading(level, text, anchor))
            if level == 1 and collect and not chapter.title:
                chapter.title = text
                out.append(f"<h1>{inline(text)}</h1>")
            else:
                out.append(f'<h{level} id="{anchor}">{inline(text)}</h{level}>')
            i += 1
            continue

        if re.fullmatch(r"(-{3,}|\*{3,}|_{3,})", stripped):
            out.append("<hr>")
            i += 1
            continue

        if stripped.startswith("|") and i + 1 < len(lines) and re.fullmatch(
            r"\|[\s:|-]+\|", lines[i + 1].strip()
        ):
            header = split_row(stripped)
            i += 2
            rows: list[list[str]] = []
            while i < len(lines) and lines[i].strip().startswith("|"):
                rows.append(split_row(lines[i].strip()))
                i += 1
            cells = "".join(f"<th>{inline(c)}</th>" for c in header)
            body = "".join(
                "<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>" for r in rows
            )
            out.append(
                f'<div class="wrap"><table><thead><tr>{cells}</tr></thead>'
                f"<tbody>{body}</tbody></table></div>"
            )
            continue

        if stripped.startswith(">"):
            quote: list[str] = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                quote.append(lines[i].strip().lstrip(">").strip())
                i += 1
            out.append("<blockquote>" + render(quote, chapter, used, collect=False) + "</blockquote>")
            continue

        if re.match(r"[-*]\s+", stripped) or re.match(r"\d+[.)]\s+", stripped):
            i, listing = render_list(lines, i, chapter, used)
            out.append(listing)
            continue

        para: list[str] = []
        while i < len(lines) and lines[i].strip() and not is_block_start(lines[i].strip()):
            para.append(lines[i].strip())
            i += 1
        out.append("<p>" + inline(join_wrapped(para)) + "</p>")

    return "\n".join(out)


def toc_for(chapter: Chapter) -> str:
    entries = [h for h in chapter.headings if h.level == 2]
    if len(entries) < 2:
        return ""
    links = "".join(f'<li><a href="#{h.anchor}">{inline(h.text)}</a></li>' for h in entries)
    return f'<nav class="toc"><strong>このページの内容</strong><ul>{links}</ul></nav>'


def build() -> int:
    sources = sorted(p for p in SRC_DIR.glob("*.md") if not p.name.startswith("_"))
    if not sources:
        print(f"no markdown sources under {SRC_DIR}", file=sys.stderr)
        return 1

    chapters: list[Chapter] = []
    for path in sources:
        chapter = Chapter(path)
        raw = path.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n")
        chapter.body = render(raw, chapter, {})
        if not chapter.title:
            print(f"{path.name}: no level-1 heading", file=sys.stderr)
            return 1
        chapters.append(chapter)

    for pos, chapter in enumerate(chapters):
        prev_link = (
            f'<a href="{chapters[pos - 1].out_name}">&laquo; {html.escape(chapters[pos - 1].title)}</a>'
            if pos
            else '<a href="index.html">&laquo; 目次</a>'
        )
        next_link = (
            f'<a href="{chapters[pos + 1].out_name}">{html.escape(chapters[pos + 1].title)} &raquo;</a>'
            if pos + 1 < len(chapters)
            else ""
        )
        page = PAGE.format(
            title=f"{chapter.title} - StirHex ヘルプ",
            css=CSS,
            home='<div class="home"><a href="index.html">StirHex ヘルプ 目次</a></div>',
            body=toc_for(chapter) + chapter.body,
            nav=f'<nav class="chapters"><span>{prev_link}</span><span>{next_link}</span></nav>',
        )
        (HELP_DIR / chapter.out_name).write_text(page, encoding="utf-8", newline="\n")

    items: list[str] = []
    for chapter in chapters:
        subs = "".join(
            f'<li><a href="{chapter.out_name}#{h.anchor}">{inline(h.text)}</a></li>'
            for h in chapter.headings
            if h.level == 2
        )
        items.append(
            f'<li><a href="{chapter.out_name}">{html.escape(chapter.title)}</a>'
            + (f"<ul>{subs}</ul>" if subs else "")
            + "</li>"
        )
    index = PAGE.format(
        title="StirHex ヘルプ",
        css=CSS,
        home="",
        body='<h1>StirHex ヘルプ</h1><nav class="toc"><ul>' + "".join(items) + "</ul></nav>",
        nav="",
    )
    (HELP_DIR / "index.html").write_text(index, encoding="utf-8", newline="\n")

    print(f"built {len(chapters)} chapter page(s) + index.html into {HELP_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(build())
