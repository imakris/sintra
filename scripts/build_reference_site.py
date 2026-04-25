#!/usr/bin/env python3
"""Build the Sintra reference Markdown into a static HTML site."""

from __future__ import annotations

import html
import os
import re
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[1]
REFERENCE_DIR = ROOT / "docs" / "reference"
GUIDE = ROOT / "docs" / "guide.md"
OUT_DIR = ROOT / "docs" / "reference_site"
ASSET_DIR = OUT_DIR / "assets"
PAGE_DIR = OUT_DIR / "pages"


@dataclass(frozen=True)
class Page:
    source: Path
    output: Path
    title: str
    body: str
    is_index: bool = False


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


def is_external_url(target: str) -> bool:
    return target.startswith(("http://", "https://", "mailto:"))


def output_for_markdown(path: Path) -> Path | None:
    resolved = path.resolve()
    if resolved == GUIDE.resolve():
        return PAGE_DIR / "guide.html"
    try:
        resolved.relative_to(REFERENCE_DIR.resolve())
    except ValueError:
        return None
    if resolved.name == "index.md":
        return OUT_DIR / "index.html"
    return PAGE_DIR / f"{resolved.stem}.html"


def rewrite_link(source: Path, output: Path, target: str) -> str:
    if is_external_url(target) or target.startswith("#"):
        return target

    target = target.strip()
    url, hash_mark, fragment = target.partition("#")
    if not url:
        return target

    resolved = (source.parent / url).resolve()
    generated = output_for_markdown(resolved)
    if generated is not None:
        rel = os.path.relpath(generated, output.parent).replace(os.sep, "/")
        return f"{rel}{hash_mark}{fragment}"

    rel = os.path.relpath(resolved, output.parent).replace(os.sep, "/")
    return f"{rel}{hash_mark}{fragment}"


def render_inline(text: str, source: Path, output: Path) -> str:
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
        label = (
            restore_placeholders(raw_label)
            if "\x00" in raw_label
            else render_inline(raw_label, source, output)
        )
        href = html.escape(rewrite_link(source, output, match.group(2)), quote=True)
        return stash(f'<a href="{href}">{label}</a>')

    text = re.sub(r"`([^`]*)`", code_repl, text)
    text = re.sub(r"(?<!!)\[([^\]]+)\]\(([^)]+)\)", link_repl, text)
    text = html.escape(text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)

    return restore_placeholders(text)


def render_list(lines: list[str], source: Path, output: Path, ordered: bool) -> str:
    tag = "ol" if ordered else "ul"
    items: list[list[str]] = []
    current: list[str] | None = None
    marker = re.compile(r"^\s*(?:[-*]\s+|\d+\.\s+)")

    for line in lines:
        if marker.match(line):
            if current is not None:
                items.append(current)
            current = [marker.sub("", line).strip()]
        elif current is not None:
            current.append(line.strip())

    if current is not None:
        items.append(current)

    rendered = []
    for item in items:
        text = " ".join(part for part in item if part)
        rendered.append(f"<li>{render_inline(text, source, output)}</li>")

    return f"<{tag}>\n" + "\n".join(rendered) + f"\n</{tag}>"


def render_markdown(source: Path, output: Path) -> tuple[str, str]:
    lines = source.read_text(encoding="utf-8").splitlines()
    html_blocks: list[str] = []
    headings: dict[str, int] = {}
    title = source.stem
    i = 0

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

        heading_match = re.match(r"^(#{1,6})\s+(.+?)\s*$", line)
        if heading_match:
            level = len(heading_match.group(1))
            raw_text = heading_match.group(2).strip()
            base = slugify(raw_text)
            count = headings.get(base, 0)
            headings[base] = count + 1
            anchor = base if count == 0 else f"{base}-{count}"
            if level == 1:
                title = re.sub(r"`([^`]*)`", r"\1", raw_text)
            rendered = render_inline(raw_text, source, output)
            html_blocks.append(f'<h{level} id="{anchor}">{rendered}</h{level}>')
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
                f"<th>{render_inline(cell, source, output)}</th>" for cell in headers
            )
            row_html = []
            for row in rows:
                cells = row + [""] * (len(headers) - len(row))
                row_html.append(
                    "<tr>"
                    + "".join(
                        f"<td>{render_inline(cell, source, output)}</td>"
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
                    break
                if re.match(r"^\s{0,3}#{1,6}\s+", next_line):
                    break
                if "|" in next_line and i + 1 < len(lines) and is_table_separator(lines[i + 1]):
                    break
                if next_line.strip().startswith("```"):
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
            html_blocks.append(render_list(list_lines, source, output, ordered))
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
            paragraph.append(next_line.strip())
            i += 1
        html_blocks.append(f"<p>{render_inline(' '.join(paragraph), source, output)}</p>")

    return title, "\n".join(html_blocks)


def collect_reference_links() -> list[tuple[str, list[tuple[str, Path, str]]]]:
    index = REFERENCE_DIR / "index.md"
    groups: list[tuple[str, list[tuple[str, Path, str]]]] = []
    current: tuple[str, list[tuple[str, Path, str]]] | None = None
    link_re = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")

    for line in index.read_text(encoding="utf-8").splitlines():
        heading = re.match(r"^##\s+(.+)$", line)
        if heading:
            current = (heading.group(1), [])
            groups.append(current)
            continue
        if current is None or not line.startswith("|"):
            continue
        for label, target in link_re.findall(line):
            url, _, fragment = target.partition("#")
            if url.endswith(".md"):
                generated = output_for_markdown((index.parent / url).resolve())
                if generated is not None:
                    current[1].append((re.sub(r"`", "", label), generated, fragment))
                break

    return [(name, links) for name, links in groups if links and name != "Headers"]


def render_sidebar(active_output: Path) -> str:
    groups = collect_reference_links()
    parts = ['<nav class="sidebar" aria-label="Reference navigation">']
    for group_name, links in groups:
        parts.append(f"<section><h2>{html.escape(group_name)}</h2><ul>")
        for label, target, fragment in links:
            href = os.path.relpath(target, active_output.parent).replace(os.sep, "/")
            if fragment:
                href = f"{href}#{fragment}"
            active = target.resolve() == active_output.resolve()
            class_attr = ' class="active"' if active else ""
            parts.append(
                f'<li><a{class_attr} href="{html.escape(href, quote=True)}">'
                f"{html.escape(label)}</a></li>"
            )
        parts.append("</ul></section>")
    parts.append("</nav>")
    return "\n".join(parts)


def page_template(page: Page) -> str:
    css = os.path.relpath(ASSET_DIR / "style.css", page.output.parent).replace(os.sep, "/")
    index_href = os.path.relpath(OUT_DIR / "index.html", page.output.parent).replace(os.sep, "/")
    guide_href = os.path.relpath(PAGE_DIR / "guide.html", page.output.parent).replace(os.sep, "/")
    readme_href = os.path.relpath(ROOT / "README.md", page.output.parent).replace(os.sep, "/")
    sidebar = render_sidebar(page.output)
    page_class = "reference-index" if page.is_index else "reference-page"
    title = html.escape(page.title)
    return f"""<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <meta name="color-scheme" content="dark" />
    <title>{title} - Sintra Reference</title>
    <link rel="stylesheet" href="{css}" />
  </head>
  <body>
    <a class="skip-nav" href="#main-content">Skip to content</a>
    <header class="site-header">
      <div class="header-inner">
        <a class="brand" href="{index_href}" aria-label="Sintra reference home">
          <span class="brand-mark" aria-hidden="true"></span>
          <span>Sintra Reference</span>
        </a>
        <nav class="top-nav" aria-label="Top navigation">
          <a href="{index_href}">Reference</a>
          <a href="{guide_href}">Guide</a>
          <a href="{readme_href}">README</a>
        </nav>
      </div>
    </header>
    <main id="main-content" class="page-shell {page_class}">
      {sidebar}
      <article class="content-prose">
        {page.body}
      </article>
    </main>
  </body>
</html>
"""


STYLE = """
:root {
  --color-bg: #111;
  --color-bg-alt: #1f1f1f;
  --color-text: #e0e0e0;
  --color-text-muted: #999;
  --color-border: #3a3a3a;
  --color-link: #6fa8c7;
  --color-link-hover: #8ec4df;
  --color-accent: #8ab4c7;
  --color-code-bg: #1e1e1e;
  --font-body: 'IBM Plex Sans', system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
  --font-mono: 'JetBrains Mono', ui-monospace, 'Cascadia Code', 'SFMono-Regular', Consolas, monospace;
  --size-xs: 0.75rem;
  --size-sm: 0.875rem;
  --size-base: 1rem;
  --size-lg: 1.125rem;
  --size-xl: 1.5rem;
  --size-2xl: 2rem;
  --space-xs: 0.25rem;
  --space-sm: 0.5rem;
  --space-md: 1rem;
  --space-lg: 2rem;
  --space-xl: 3rem;
  --max-width: 92rem;
  --header-height: 3.5rem;
}

* { box-sizing: border-box; }

html {
  background: var(--color-bg);
  color: var(--color-text);
}

body {
  margin: 0;
  font-family: var(--font-body);
  font-size: var(--size-base);
  line-height: 1.6;
  color: var(--color-text);
  background: rgba(17, 17, 17, 0.85);
}

a {
  color: var(--color-link);
  text-decoration: underline;
  text-underline-offset: 0.15em;
}

a:hover { color: var(--color-link-hover); }

:focus-visible {
  outline: 2px solid var(--color-link);
  outline-offset: 2px;
}

.skip-nav {
  position: absolute;
  left: -9999px;
  top: auto;
  width: 1px;
  height: 1px;
  overflow: hidden;
  z-index: 100;
  padding: var(--space-sm) var(--space-md);
  background: var(--color-bg);
  border: 2px solid var(--color-link);
}

.skip-nav:focus {
  position: fixed;
  left: var(--space-md);
  top: var(--space-md);
  width: auto;
  height: auto;
}

.site-header {
  position: sticky;
  top: 0;
  z-index: 10;
  min-height: var(--header-height);
  background: var(--color-bg);
  border-bottom: 1px solid var(--color-border);
}

.header-inner {
  max-width: var(--max-width);
  min-height: var(--header-height);
  margin: 0 auto;
  padding: 0 var(--space-md);
  display: flex;
  align-items: center;
  gap: var(--space-lg);
}

.brand {
  display: inline-flex;
  align-items: center;
  gap: var(--space-sm);
  color: var(--color-text);
  text-decoration: none;
  font-weight: 600;
}

.brand-mark {
  width: 0.85rem;
  height: 0.85rem;
  display: inline-block;
  background: rgb(196, 77, 40);
}

.top-nav {
  display: flex;
  gap: var(--space-md);
  margin-left: auto;
}

.top-nav a {
  color: var(--color-text-muted);
  font-size: var(--size-sm);
  text-decoration: none;
}

.top-nav a:hover { color: var(--color-text); }

.page-shell {
  width: 100%;
  max-width: var(--max-width);
  margin: 0 auto;
  padding: var(--space-lg) var(--space-md);
  display: grid;
  grid-template-columns: minmax(12rem, 16rem) minmax(0, 1fr);
  gap: var(--space-xl);
}

.sidebar {
  position: sticky;
  top: calc(var(--header-height) + var(--space-lg));
  align-self: start;
  max-height: calc(100vh - var(--header-height) - var(--space-xl));
  overflow: auto;
  padding-right: var(--space-md);
  border-right: 1px solid var(--color-border);
}

.sidebar section + section { margin-top: var(--space-lg); }

.sidebar h2 {
  margin: 0 0 var(--space-xs);
  font-family: var(--font-mono);
  font-size: var(--size-xs);
  font-weight: 400;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: var(--color-text-muted);
}

.sidebar ul {
  list-style: none;
  padding: 0;
  margin: 0;
}

.sidebar li { margin: 0; }

.sidebar a {
  display: block;
  padding: 0.18rem 0;
  color: var(--color-text-muted);
  font-size: var(--size-sm);
  text-decoration: none;
}

.sidebar a:hover,
.sidebar a.active {
  color: var(--color-link-hover);
}

.content-prose {
  max-width: 56rem;
  line-height: 1.7;
}

.reference-index .content-prose {
  max-width: none;
}

h1, h2, h3, h4 {
  line-height: 1.3;
  color: var(--color-text);
  margin-top: var(--space-lg);
  margin-bottom: var(--space-sm);
}

h1 {
  font-size: var(--size-2xl);
  margin-top: 0;
}

h2 {
  font-size: var(--size-xl);
  font-weight: 500;
  margin-top: var(--space-xl);
  padding-top: var(--space-md);
  border-top: 1px solid var(--color-border);
}

h1 + h2,
h2:first-child {
  border-top: none;
  padding-top: 0;
}

h3 {
  font-size: var(--size-lg);
  font-weight: 500;
}

h4 {
  font-size: var(--size-base);
  font-weight: 500;
  color: var(--color-text-muted);
}

p { margin: 0 0 var(--space-md); }

ul, ol {
  padding-left: var(--space-lg);
  margin: 0 0 var(--space-md);
}

li { margin-bottom: var(--space-xs); }

code {
  font-family: var(--font-mono);
  font-size: var(--size-sm);
  background: var(--color-code-bg);
  padding: 0.15em 0.35em;
  border-radius: 3px;
}

pre {
  background: var(--color-code-bg);
  padding: var(--space-md);
  border-radius: 4px;
  overflow-x: auto;
  margin: 0 0 var(--space-md);
  border: 1px solid var(--color-border);
}

pre code {
  background: none;
  padding: 0;
}

.table-wrap {
  overflow-x: auto;
  margin: var(--space-md) 0;
}

table {
  width: 100%;
  border-collapse: collapse;
  font-size: var(--size-sm);
}

th, td {
  border: 1px solid var(--color-border);
  padding: var(--space-sm) var(--space-md);
  text-align: left;
  vertical-align: top;
}

th {
  background: var(--color-bg-alt);
  font-weight: 600;
}

blockquote {
  border-left: 3px solid var(--color-accent);
  padding: var(--space-sm) var(--space-md);
  background: color-mix(in srgb, var(--color-accent) 6%, transparent);
  color: var(--color-text-muted);
  border-radius: 0 4px 4px 0;
  font-size: var(--size-sm);
  line-height: 1.6;
  margin: var(--space-md) 0;
}

@media (max-width: 54rem) {
  .header-inner {
    flex-wrap: wrap;
    padding-top: var(--space-sm);
    padding-bottom: var(--space-sm);
  }

  .top-nav {
    width: 100%;
    margin-left: 0;
  }

  .page-shell {
    grid-template-columns: 1fr;
    gap: var(--space-lg);
  }

  .sidebar {
    position: static;
    max-height: none;
    border-right: none;
    border-bottom: 1px solid var(--color-border);
    padding-right: 0;
    padding-bottom: var(--space-md);
  }

  .sidebar section {
    margin-top: var(--space-md);
  }
}
"""


def build_pages() -> Iterable[Page]:
    sources = [REFERENCE_DIR / "index.md"]
    sources.extend(path for path in sorted(REFERENCE_DIR.glob("*.md")) if path.name != "index.md")
    sources.append(GUIDE)

    for source in sources:
        output = output_for_markdown(source)
        if output is None:
            continue
        title, body = render_markdown(source, output)
        yield Page(source=source, output=output, title=title, body=body, is_index=source.name == "index.md")


def main() -> None:
    if OUT_DIR.exists():
        shutil.rmtree(OUT_DIR)
    ASSET_DIR.mkdir(parents=True)
    PAGE_DIR.mkdir(parents=True)
    (ASSET_DIR / "style.css").write_text(STYLE.strip() + "\n", encoding="utf-8")

    for page in build_pages():
        page.output.write_text(page_template(page), encoding="utf-8")

    print(f"Built reference site at {OUT_DIR}")


if __name__ == "__main__":
    main()
