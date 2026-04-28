#!/usr/bin/env python3
"""Build the Sintra reference Markdown into a static HTML site.

The site has three page kinds:
  - Index (`docs/reference/index.md`)        -> single-column landing page.
  - Reference page (every other `*.md`)      -> article + right-rail "On this page" TOC.
  - Top-level docs (`docs/*.md`, `README.md`) -> rendered as guide-style pages.

Layout features (driven from the markdown sources, no JS):
  - Breadcrumb row under the header on every non-index page.
  - Right-rail on-page TOC built from H2/H3 of the rendered article.
  - Prev / Next links derived from the order of entries in `index.md`.
  - Build-time link checker that fails on broken cross-references.

CSS lives in `scripts/site_assets/style.css` and is copied into the output
on every build. There is no inlined CSS in this script.
"""

from __future__ import annotations

import html
import os
import re
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import quote


ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = ROOT / "docs"
REFERENCE_DIR = DOCS_DIR / "reference"
README = ROOT / "README.md"
OUT_DIR = DOCS_DIR / "reference_site"
ASSET_DIR = OUT_DIR / "assets"
PAGE_DIR = OUT_DIR / "pages"
SITE_ASSETS_SRC = Path(__file__).resolve().parent / "site_assets"
GITHUB_BLOB_BASE = "https://github.com/imakris/sintra/blob/master"


# ============================================================================
# Data types
# ============================================================================


@dataclass(frozen=True)
class Heading:
    level: int
    anchor: str
    text_html: str


@dataclass(frozen=True)
class CollectedLink:
    source: Path           # source markdown file the link was written in
    output: Path           # generated HTML the rewritten href is relative to
    raw_target: str        # the link target exactly as it appears in markdown


@dataclass
class Page:
    source: Path
    output: Path
    title: str
    body: str
    headings: list[Heading]
    links: list[CollectedLink]
    page_class: str
    section: str | None        # H2 group from index.md, or None for top-level pages
    label: str | None          # display label used in breadcrumbs and prev/next
    is_index: bool = False


@dataclass(frozen=True)
class IndexLink:
    label: str             # plain-text label (no markdown markup)
    output: Path           # absolute output path
    fragment: str          # anchor inside that page, may be ""


# ============================================================================
# Generic helpers
# ============================================================================


def slugify(text: str) -> str:
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.strip().lower()
    text = re.sub(r"[^a-z0-9 _-]", "", text)
    text = re.sub(r"\s+", "-", text)
    return text


def split_table_row(line: str) -> list[str]:
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|"):
        line = line[:-1]
    return [cell.strip() for cell in line.split("|")]


def is_table_separator(line: str) -> bool:
    cells = split_table_row(line)
    return bool(cells) and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells)


def is_thematic_break(line: str) -> bool:
    return bool(re.fullmatch(r"\s{0,3}(?:-{3,}|\*{3,}|_{3,})\s*", line))


def is_external_url(target: str) -> bool:
    return target.startswith(("http://", "https://", "mailto:", "tel:", "ftp://"))


def output_for_markdown(path: Path) -> Path | None:
    """Map a markdown source path to its generated HTML, or None if not in the site."""
    resolved = path.resolve()
    if resolved == README.resolve():
        return PAGE_DIR / "readme.html"
    # Reference pages: docs/reference/<stem>.md -> docs/reference_site/{index|pages/<stem>}.html
    try:
        resolved.relative_to(REFERENCE_DIR.resolve())
    except ValueError:
        pass
    else:
        if resolved.name == "index.md":
            return OUT_DIR / "index.html"
        return PAGE_DIR / f"{resolved.stem}.html"
    # Top-level docs: docs/<stem>.md -> docs/reference_site/pages/<stem>.html
    try:
        rel = resolved.relative_to(DOCS_DIR.resolve())
    except ValueError:
        return None
    if len(rel.parts) == 1 and rel.suffix == ".md":
        return PAGE_DIR / f"{resolved.stem}.html"
    return None


def relpath(target: Path, start: Path) -> str:
    return os.path.relpath(target, start).replace(os.sep, "/")


def repo_blob_url(target: Path, fragment: str = "") -> str | None:
    try:
        rel = target.resolve().relative_to(ROOT.resolve())
    except ValueError:
        return None
    suffix = f"#{fragment}" if fragment else ""
    return f"{GITHUB_BLOB_BASE}/{quote(rel.as_posix(), safe='/')}{suffix}"


# ============================================================================
# Markdown rendering
# ============================================================================


def rewrite_link(source: Path, output: Path, target: str) -> str:
    target = target.strip()
    if is_external_url(target) or target.startswith("#"):
        return target

    url, hash_mark, fragment = target.partition("#")
    if not url:
        return target

    resolved = (source.parent / url).resolve()
    generated = output_for_markdown(resolved)
    if generated is not None:
        rel = relpath(generated, output.parent)
        return f"{rel}{hash_mark}{fragment}"

    blob_url = repo_blob_url(resolved, fragment)
    if blob_url is not None:
        return blob_url

    rel = relpath(resolved, output.parent)
    return f"{rel}{hash_mark}{fragment}"


def render_inline(
    text: str,
    source: Path,
    output: Path,
    links: list[CollectedLink] | None = None,
) -> str:
    placeholders: list[str] = []

    def stash(value: str) -> str:
        placeholders.append(value)
        return f"\x00{len(placeholders) - 1}\x00"

    def code_repl(match: re.Match[str]) -> str:
        return stash(f"<code>{html.escape(match.group(1))}</code>")

    def restore_placeholders(value: str) -> str:
        return re.sub(
            r"\x00(\d+)\x00",
            lambda match: placeholders[int(match.group(1))],
            value,
        )

    def link_repl(match: re.Match[str]) -> str:
        raw_label = match.group(1)
        raw_target = match.group(2).strip()
        label = (
            restore_placeholders(raw_label)
            if "\x00" in raw_label
            else render_inline(raw_label, source, output, links)
        )
        if links is not None:
            links.append(CollectedLink(source=source, output=output, raw_target=raw_target))
        href = html.escape(rewrite_link(source, output, raw_target), quote=True)
        return stash(f'<a href="{href}">{label}</a>')

    def image_repl(match: re.Match[str]) -> str:
        alt = match.group(1)
        raw_target = match.group(2).strip()
        if is_external_url(raw_target) or raw_target.startswith("#"):
            src = raw_target
        else:
            src = rewrite_link(source, output, raw_target)
        src_attr = html.escape(src, quote=True)
        alt_attr = html.escape(alt, quote=True)
        return stash(f'<img src="{src_attr}" alt="{alt_attr}" />')

    text = re.sub(r"`([^`]*)`", code_repl, text)
    text = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", image_repl, text)
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link_repl, text)
    text = html.escape(text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)

    return restore_placeholders(text)


def render_list(
    lines: list[str],
    source: Path,
    output: Path,
    ordered: bool,
    links: list[CollectedLink],
) -> str:
    marker = re.compile(r"^(\s*)([-*]|\d+\.)\s+(.*)$")

    def line_indent(value: str) -> int:
        expanded = value.expandtabs(4)
        return len(expanded) - len(expanded.lstrip(" "))

    def marker_info(value: str) -> tuple[int, bool, str] | None:
        match = marker.match(value)
        if not match:
            return None
        token = match.group(2)
        return line_indent(match.group(1)), token.endswith("."), match.group(3).strip()

    def render_item(parts: list[str]) -> str:
        inline_parts = [p for p in parts if not p.startswith(("<ul", "<ol"))]
        nested_parts = [p for p in parts if p.startswith(("<ul", "<ol"))]
        content = " ".join(p for p in inline_parts if p)
        if nested_parts:
            content = "\n".join([content, *nested_parts]) if content else "\n".join(nested_parts)
        return f"<li>{content}</li>"

    def parse_level(index: int, indent: int, level_ordered: bool) -> tuple[str, int]:
        items: list[list[str]] = []
        current: list[str] | None = None

        while index < len(lines):
            if not lines[index].strip():
                index += 1
                continue

            info = marker_info(lines[index])
            if info is None:
                if current is not None:
                    current.append(render_inline(lines[index].strip(), source, output, links))
                index += 1
                continue

            item_indent, item_ordered, item_text = info
            if item_indent < indent:
                break
            if item_indent > indent:
                if current is None:
                    current = []
                nested_html, index = parse_level(index, item_indent, item_ordered)
                current.append(nested_html)
                continue

            if current is not None:
                items.append(current)
            current = [render_inline(item_text, source, output, links)]
            index += 1

        if current is not None:
            items.append(current)

        tag = "ol" if level_ordered else "ul"
        body = "\n".join(render_item(item) for item in items)
        return f"<{tag}>\n{body}\n</{tag}>", index

    first = marker_info(lines[0])
    start_indent = first[0] if first is not None else 0
    rendered, _ = parse_level(0, start_indent, ordered)
    return rendered


def render_markdown(
    source: Path, output: Path
) -> tuple[str, str, list[Heading], list[CollectedLink]]:
    # `utf-8-sig` strips a leading BOM if present (the repo README has one).
    lines = source.read_text(encoding="utf-8-sig").splitlines()
    html_blocks: list[str] = []
    headings: list[Heading] = []
    links: list[CollectedLink] = []
    seen_anchors: dict[str, int] = {}
    title = source.stem
    i = 0

    # CommonMark Type 6 HTML block: line begins with `<` or `</` followed by a
    # block-level tag; the block runs until the next blank line.
    html_block_open = re.compile(r"^\s{0,3}</?[a-zA-Z][a-zA-Z0-9-]*(\s|/?>|$)")

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if not stripped:
            i += 1
            continue

        if stripped.startswith("```"):
            language = stripped[3:].strip()
            code_lines: list[str] = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith("```"):
                code_lines.append(lines[i])
                i += 1
            i += 1
            class_attr = f' class="language-{html.escape(language)}"' if language else ""
            code = html.escape("\n".join(code_lines))
            html_blocks.append(f"<pre><code{class_attr}>{code}</code></pre>")
            continue

        if html_block_open.match(line):
            raw_lines: list[str] = [line]
            i += 1
            while i < len(lines) and lines[i].strip():
                raw_lines.append(lines[i])
                i += 1
            html_blocks.append("\n".join(raw_lines))
            continue

        if is_thematic_break(line):
            html_blocks.append("<hr>")
            i += 1
            continue

        heading_match = re.match(r"^(#{1,6})\s+(.+?)\s*$", line)
        if heading_match:
            level = len(heading_match.group(1))
            raw_text = heading_match.group(2).strip()
            base = slugify(raw_text)
            count = seen_anchors.get(base, 0)
            seen_anchors[base] = count + 1
            anchor = base if count == 0 else f"{base}-{count}"
            if level == 1:
                title = re.sub(r"`([^`]*)`", r"\1", raw_text)
            rendered = render_inline(raw_text, source, output, links)
            html_blocks.append(f'<h{level} id="{anchor}">{rendered}</h{level}>')
            headings.append(Heading(level=level, anchor=anchor, text_html=rendered))
            i += 1
            continue

        if (
            "|" in line
            and i + 1 < len(lines)
            and "|" in lines[i + 1]
            and is_table_separator(lines[i + 1])
        ):
            headers = split_table_row(line)
            i += 2
            rows: list[list[str]] = []
            while i < len(lines) and "|" in lines[i].strip() and lines[i].strip():
                rows.append(split_table_row(lines[i]))
                i += 1
            header_html = "".join(
                f"<th>{render_inline(cell, source, output, links)}</th>" for cell in headers
            )
            row_html = []
            for row in rows:
                cells = row + [""] * (len(headers) - len(row))
                row_html.append(
                    "<tr>"
                    + "".join(
                        f"<td>{render_inline(cell, source, output, links)}</td>"
                        for cell in cells[: len(headers)]
                    )
                    + "</tr>"
                )
            html_blocks.append(
                '<div class="table-wrap"><table><thead><tr>'
                + header_html
                + "</tr></thead><tbody>"
                + "".join(row_html)
                + "</tbody></table></div>"
            )
            continue

        if re.match(r"^\s*[-*]\s+", line) or re.match(r"^\s*\d+\.\s+", line):
            ordered = bool(re.match(r"^\s*\d+\.\s+", line))
            list_lines = [line]
            i += 1
            while i < len(lines):
                next_line = lines[i]
                if not next_line.strip():
                    lookahead = i + 1
                    while lookahead < len(lines) and not lines[lookahead].strip():
                        lookahead += 1
                    if lookahead < len(lines) and (
                        re.match(r"^\s*[-*]\s+", lines[lookahead])
                        or re.match(r"^\s*\d+\.\s+", lines[lookahead])
                        or lines[lookahead].startswith(("  ", "   ", "    "))
                    ):
                        list_lines.append("")
                        i = lookahead
                        continue
                    break
                if re.match(r"^\s{0,3}#{1,6}\s+", next_line):
                    break
                if "|" in next_line and i + 1 < len(lines) and is_table_separator(lines[i + 1]):
                    break
                if next_line.strip().startswith("```"):
                    break
                if is_thematic_break(next_line):
                    break
                if (
                    re.match(r"^\s*[-*]\s+", next_line)
                    or re.match(r"^\s*\d+\.\s+", next_line)
                    or next_line.startswith(("  ", "   ", "    "))
                ):
                    list_lines.append(next_line)
                    i += 1
                    continue
                break
            html_blocks.append(render_list(list_lines, source, output, ordered, links))
            continue

        paragraph = [line.strip()]
        i += 1
        while i < len(lines):
            next_line = lines[i]
            if not next_line.strip():
                break
            if next_line.strip().startswith("```"):
                break
            if re.match(r"^\s{0,3}#{1,6}\s+", next_line):
                break
            if re.match(r"^\s*[-*]\s+", next_line) or re.match(r"^\s*\d+\.\s+", next_line):
                break
            if "|" in next_line and i + 1 < len(lines) and is_table_separator(lines[i + 1]):
                break
            if is_thematic_break(next_line):
                break
            paragraph.append(next_line.strip())
            i += 1
        html_blocks.append(
            f"<p>{render_inline(' '.join(paragraph), source, output, links)}</p>"
        )

    return title, "\n".join(html_blocks), headings, links


# ============================================================================
# Index ordering: parse index.md to drive breadcrumbs and prev/next
# ============================================================================


def parse_index_order() -> tuple[list[IndexLink], dict[Path, IndexLink], dict[Path, str]]:
    """Return the linear page order (deduped by output path), a per-page lookup,
    and a per-page section name."""
    index_md = REFERENCE_DIR / "index.md"
    sequence: list[IndexLink] = []
    seen: set[Path] = set()
    by_output: dict[Path, IndexLink] = {}
    section_for: dict[Path, str] = {}
    current_section: str | None = None
    link_re = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")

    for line in index_md.read_text(encoding="utf-8").splitlines():
        heading = re.match(r"^##\s+(.+)$", line)
        if heading:
            current_section = heading.group(1).strip()
            continue
        if current_section is None or not line.startswith("|") or current_section == "Headers":
            continue
        for label, target in link_re.findall(line):
            url, _, fragment = target.partition("#")
            if not url.endswith(".md"):
                continue
            generated = output_for_markdown((index_md.parent / url).resolve())
            if generated is None:
                continue
            entry = IndexLink(
                label=re.sub(r"`", "", label).strip(),
                output=generated.resolve(),
                fragment=fragment,
            )
            section_for.setdefault(entry.output, current_section)
            if entry.output in seen:
                break
            seen.add(entry.output)
            if not entry.fragment:
                sequence.append(entry)
                by_output[entry.output] = entry
            else:
                synthetic = IndexLink(label=entry.label, output=entry.output, fragment="")
                sequence.append(synthetic)
                by_output[entry.output] = synthetic
            break

    return sequence, by_output, section_for


# ============================================================================
# HTML fragments: header, breadcrumb, on-page TOC, prev/next
# ============================================================================


def render_header(page: Page) -> str:
    output_dir = page.output.parent
    index_href = relpath(OUT_DIR / "index.html", output_dir)
    guide_href = relpath(PAGE_DIR / "guide.html", output_dir)
    readme_href = relpath(PAGE_DIR / "readme.html", output_dir)

    def nav_attr(target: Path) -> str:
        return ' aria-current="page"' if target.resolve() == page.output.resolve() else ""

    return f"""\
    <header class="site-header">
      <div class="header-inner">
        <a class="brand" href="{index_href}" aria-label="Sintra reference home">
          <span class="brand-mark" aria-hidden="true"></span>
          <span>Sintra Reference</span>
        </a>
        <nav class="top-nav" aria-label="Top navigation">
          <a href="{index_href}"{nav_attr(OUT_DIR / "index.html")}>Reference</a>
          <a href="{guide_href}"{nav_attr(PAGE_DIR / "guide.html")}>Guide</a>
          <a href="{readme_href}"{nav_attr(PAGE_DIR / "readme.html")}>README</a>
        </nav>
      </div>
    </header>"""


def render_breadcrumb(page: Page) -> str:
    if page.is_index:
        return ""
    output_dir = page.output.parent
    index_href = relpath(OUT_DIR / "index.html", output_dir)
    crumbs: list[str] = [
        f'<li><a href="{index_href}">Reference</a></li>'
    ]
    if page.section:
        section_anchor = slugify(page.section)
        crumbs.append(
            f'<li><a href="{index_href}#{section_anchor}">{html.escape(page.section)}</a></li>'
        )
    label = page.label or page.title
    crumbs.append(f'<li aria-current="page">{html.escape(label)}</li>')
    return (
        '    <nav class="breadcrumb" aria-label="Breadcrumb">\n'
        f'      <ol>{"".join(crumbs)}</ol>\n'
        '    </nav>'
    )


def render_on_page_toc(page: Page) -> str:
    if page.is_index:
        return ""
    items = [h for h in page.headings if h.level in (2, 3)]
    if not items:
        return ""
    parts = [
        '<aside class="page-toc" aria-label="On this page">',
        '  <h2>On this page</h2>',
        '  <ul>',
    ]
    for h in items:
        parts.append(
            f'    <li class="level-{h.level}">'
            f'<a href="#{h.anchor}">{h.text_html}</a></li>'
        )
    parts.append('  </ul>')
    parts.append('</aside>')
    return "\n".join(parts)


def render_page_nav(page: Page, sequence: list[IndexLink]) -> str:
    if page.is_index or not sequence:
        return ""
    target_resolved = page.output.resolve()
    pos = next(
        (i for i, e in enumerate(sequence) if e.output == target_resolved),
        None,
    )
    if pos is None:
        return ""
    output_dir = page.output.parent

    def link_block(entry: IndexLink, kind: str, label: str) -> str:
        href = relpath(entry.output, output_dir)
        return (
            f'<a class="page-nav-{kind}" href="{href}">'
            f'<span class="label">{label}</span>'
            f'<span class="title">{html.escape(entry.label)}</span>'
            f'</a>'
        )

    prev = sequence[pos - 1] if pos > 0 else None
    nxt = sequence[pos + 1] if pos + 1 < len(sequence) else None

    if prev is None and nxt is None:
        return ""

    cells = []
    if prev is not None:
        cells.append(link_block(prev, "prev", "&larr; Previous"))
    else:
        cells.append('<span class="page-nav-spacer" aria-hidden="true"></span>')
    if nxt is not None:
        cells.append(link_block(nxt, "next", "Next &rarr;"))
    else:
        cells.append('<span class="page-nav-spacer" aria-hidden="true"></span>')

    return f'<nav class="page-nav" aria-label="Reference navigation">{"".join(cells)}</nav>'


def page_template(page: Page, sequence: list[IndexLink]) -> str:
    css = relpath(ASSET_DIR / "style.css", page.output.parent)
    title = html.escape(page.title)
    header = render_header(page)
    breadcrumb = render_breadcrumb(page)
    toc = render_on_page_toc(page)
    nav_bottom = render_page_nav(page, sequence)
    body_class = page.page_class
    if not page.is_index and not toc:
        # No H2/H3 in the article -> collapse the right rail and use the
        # full content width.
        body_class = f"{body_class} no-toc"

    def indent_block(value: str, spaces: int) -> str:
        prefix = " " * spaces
        return "\n".join(f"{prefix}{line}" if line else "" for line in value.splitlines())

    article_parts = [page.body] if page.is_index else [
        part for part in (page.body, nav_bottom) if part
    ]
    article = "\n".join(article_parts)
    main_lines = [
        '    <main id="main-content" class="page-shell">',
        '      <article class="content-prose">',
        indent_block(article, 8),
        '      </article>',
    ]
    if not page.is_index and toc:
        main_lines.append(indent_block(toc, 6))
    main_lines.append('    </main>')
    main = "\n".join(main_lines)

    pieces = [
        '<!doctype html>',
        '<html lang="en">',
        '  <head>',
        '    <meta charset="utf-8" />',
        '    <meta name="viewport" content="width=device-width, initial-scale=1" />',
        '    <meta name="color-scheme" content="dark" />',
        f'    <title>{title} - Sintra Reference</title>',
        f'    <link rel="stylesheet" href="{css}" />',
        '  </head>',
        f'  <body class="{body_class}">',
        '    <a class="skip-nav" href="#main-content">Skip to content</a>',
        header,
        breadcrumb,
        main,
        '  </body>',
        '</html>',
        '',
    ]
    return "\n".join(p for p in pieces if p is not None)


# ============================================================================
# Build pipeline
# ============================================================================


def discover_sources() -> list[Path]:
    sources: list[Path] = [REFERENCE_DIR / "index.md"]
    sources.extend(p for p in sorted(REFERENCE_DIR.glob("*.md")) if p.name != "index.md")
    sources.extend(sorted(DOCS_DIR.glob("*.md")))
    if README.is_file():
        sources.append(README)
    return sources


def build_pages() -> list[Page]:
    sequence, by_output, section_for = parse_index_order()
    pages: list[Page] = []
    for source in discover_sources():
        output = output_for_markdown(source)
        if output is None:
            continue
        title, body, headings, links = render_markdown(source, output)
        is_index = source.name == "index.md"
        if is_index:
            page_class = "reference-index"
            section = None
            label = title
        elif source.parent == DOCS_DIR or source == README:
            # Top-level docs (guide, architecture, barriers_and_shutdown,
            # process_lifecycle_notes) and the repo README sit directly
            # under the Reference root in the breadcrumb hierarchy; they
            # are not nested under any index H2.
            page_class = "reference-guide"
            section = None
            label = title
        else:
            page_class = "reference-page"
            entry = by_output.get(output.resolve())
            section = section_for.get(output.resolve())
            label = entry.label if entry else title
        pages.append(
            Page(
                source=source,
                output=output,
                title=title,
                body=body,
                headings=headings,
                links=links,
                is_index=is_index,
                page_class=page_class,
                section=section,
                label=label,
            )
        )
    return pages


# ============================================================================
# Link checker
# ============================================================================


@dataclass
class LinkProblem:
    source: Path
    target: str
    reason: str


def check_links(pages: list[Page]) -> list[LinkProblem]:
    """Return a list of broken or dangling links across all rendered pages."""
    anchors_by_output: dict[Path, set[str]] = {}
    for page in pages:
        anchors_by_output[page.output.resolve()] = {h.anchor for h in page.headings}

    problems: list[LinkProblem] = []
    for page in pages:
        for link in page.links:
            target = link.raw_target.strip()
            if not target:
                problems.append(
                    LinkProblem(link.source, link.raw_target, "empty link target")
                )
                continue
            if is_external_url(target):
                continue
            url, _, fragment = target.partition("#")

            # Same-page fragment (`#anchor`).
            if not url:
                if not fragment:
                    problems.append(
                        LinkProblem(
                            link.source,
                            link.raw_target,
                            "dead in-page anchor `#` with no fragment",
                        )
                    )
                    continue
                anchors = anchors_by_output.get(page.output.resolve(), set())
                if fragment not in anchors:
                    problems.append(
                        LinkProblem(
                            link.source,
                            link.raw_target,
                            f"anchor `#{fragment}` not found in {page.source.name}",
                        )
                    )
                continue

            resolved = (link.source.parent / url).resolve()
            generated = output_for_markdown(resolved)
            if generated is not None:
                if not generated.exists():
                    problems.append(
                        LinkProblem(
                            link.source,
                            link.raw_target,
                            f"links to markdown {url!r} but no generated page exists",
                        )
                    )
                    continue
                if fragment:
                    anchors = anchors_by_output.get(generated.resolve(), set())
                    if fragment not in anchors:
                        problems.append(
                            LinkProblem(
                                link.source,
                                link.raw_target,
                                f"anchor `#{fragment}` not found in {resolved.name}",
                            )
                        )
                continue

            # Out-of-tree relative target (e.g. ../../example/foo.cpp). Just
            # check the file exists.
            if not resolved.exists():
                problems.append(
                    LinkProblem(
                        link.source,
                        link.raw_target,
                        f"relative path does not exist on disk: {resolved}",
                    )
                )
    return problems


# ============================================================================
# Site assembly
# ============================================================================


def copy_static_assets() -> None:
    if not SITE_ASSETS_SRC.is_dir():
        raise SystemExit(
            f"Static asset directory not found: {SITE_ASSETS_SRC}.\n"
            "Expected `scripts/site_assets/style.css` to exist."
        )
    for entry in SITE_ASSETS_SRC.iterdir():
        if entry.is_file():
            shutil.copy2(entry, ASSET_DIR / entry.name)


def main() -> int:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    ASSET_DIR.mkdir(parents=True)
    PAGE_DIR.mkdir(parents=True)
    copy_static_assets()

    pages = build_pages()
    sequence, _by_output, _section_for = parse_index_order()

    for page in pages:
        page.output.write_text(page_template(page, sequence), encoding="utf-8")

    problems = check_links(pages)
    if problems:
        sys.stderr.write("Broken or dangling links:\n")
        for p in problems:
            rel = p.source.relative_to(ROOT)
            sys.stderr.write(f"  {rel}: {p.target!r} -> {p.reason}\n")
        sys.stderr.write(f"\n{len(problems)} link problem(s).\n")
        return 1

    print(f"Built reference site at {OUT_DIR}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
