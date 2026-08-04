# SPDX-FileCopyrightText: 2026 ludable
# SPDX-License-Identifier: AGPL-3.0-only

"""Pre-build: inline web-src/<page>/ into web/<page>/index.html.

For each index.html under web-src/, this resolves <link data-inline> and
<script data-inline> tags by inlining the referenced file's contents in
place. HTML and CSS comments are stripped, as are each project-authored JS
file's leading SPDX lines. One SPDX block is emitted for the generated page;
the remaining JS content is passed through unchanged.

The output mirrors the web-src/ tree under web/, where embed_web.py picks
it up and gzips each file as a static asset. Generated HTML files carry a
"do not edit" banner; only files under web-src/ should be edited by hand.

Outputs are only rewritten when sources change (tracked via a stamp file)
so incremental builds don't churn — and so embed_web.py's own stamp stays
warm when nothing under web-src/ changed.

Runs as a PlatformIO pre-build hook (via the Import("env") mechanism) or
standalone (`python3 scripts/build_web.py`) for offline iteration.
"""

import hashlib
import re
from pathlib import Path

try:
    Import("env")  # noqa: F821  (PlatformIO injects this)
    PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
    PIO_ENV = env  # noqa: F821
except NameError:
    PROJECT_DIR = Path(__file__).resolve().parent.parent
    PIO_ENV = None

SRC_DIR = PROJECT_DIR / "web-src"
OUT_DIR = PROJECT_DIR / "web"
# Stamp lives outside OUT_DIR so embed_web.py doesn't embed it as a
# served asset (it walks every file under web/).
STAMP = PROJECT_DIR / ".build_web.stamp"
# Resolve our own path via PROJECT_DIR — when SCons runs us through
# exec(compile(...)), __file__ isn't defined.
SELF = PROJECT_DIR / "scripts" / "build_web.py"
VERSION_FILE = PROJECT_DIR / "VERSION"
VERSION_TOKEN = "{{PUMP_BUG_VERSION}}"
VERSION_RE = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?$"
)
BASE_VERSION = VERSION_FILE.read_text().strip()
if not VERSION_RE.fullmatch(BASE_VERSION):
    raise ValueError(f"invalid firmware version in {VERSION_FILE}: {BASE_VERSION!r}")
FIRMWARE_VERSION = BASE_VERSION
if PIO_ENV is not None:
    metadata = PIO_ENV.GetProjectOption("custom_firmware_version_metadata", "")
    if metadata:
        FIRMWARE_VERSION += (
            f".{metadata}" if "+" in FIRMWARE_VERSION else f"+{metadata}"
        )
if not VERSION_RE.fullmatch(FIRMWARE_VERSION):
    raise ValueError(f"invalid derived firmware version: {FIRMWARE_VERSION!r}")

LINK_TAG_RE = re.compile(
    r"<link\s+(?P<attrs>[^>]*?)\s*/?>",
    re.IGNORECASE,
)
SCRIPT_TAG_RE = re.compile(
    r"<script\s+(?P<attrs>[^>]*?)>\s*</script>",
    re.IGNORECASE,
)
ATTR_RE = re.compile(
    r'''([\w-]+)\s*=\s*"([^"]*)"'''
    r"""|([\w-]+)\s*=\s*'([^']*)'"""
    r"""|([\w-]+)"""
)
# Preserve `<!--[if ...]>...<![endif]-->` conditional comments just in case;
# everything else is fair game.
HTML_COMMENT_RE = re.compile(r"<!--(?!\[if)[\s\S]*?-->")
DOCTYPE_RE = re.compile(r"<!doctype[^>]*>", re.IGNORECASE)
# `@import "x.css";` / `@import url('x.css');` — resolved at build time so
# shared CSS (web-src/shared/) is deduped at the source while each page
# still ships as one self-contained inlined asset.
CSS_IMPORT_RE = re.compile(
    r"""@import\s+(?:url\(\s*)?["']([^"')]+)["']\s*\)?\s*;""",
    re.IGNORECASE,
)
PROJECT_JS_SPDX_RE = re.compile(
    r"\A// SPDX-FileCopyrightText: 20\d{2}(?:-20\d{2})? ludable\n"
    r"// SPDX-License-Identifier: AGPL-3\.0-only\n\n?"
)


def parse_attrs(s):
    out = {}
    for m in ATTR_RE.finditer(s):
        if m.group(1):
            out[m.group(1).lower()] = m.group(2)
        elif m.group(3):
            out[m.group(3).lower()] = m.group(4)
        elif m.group(5):
            out[m.group(5).lower()] = ""
    return out


def strip_css_comments(text: str) -> str:
    """Strip /* ... */ comments, skipping over string literals. Preserves
    /*! ... */ (license/preserve markers)."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                d = text[i]
                out.append(d)
                if d == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if d == quote:
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            if i + 2 < n and text[i + 2] == "!":
                end = text.find("*/", i + 3)
                if end == -1:
                    out.append(text[i:])
                    i = n
                else:
                    out.append(text[i : end + 2])
                    i = end + 2
                continue
            end = text.find("*/", i + 2)
            if end == -1:
                i = n
                continue
            i = end + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def resolve_css_imports(path: Path, seen=None) -> str:
    """Inline `@import` references recursively, resolving paths relative to
    the importing file. The seen-set de-dupes a file imported from multiple
    places and guards against import cycles. Position is irrelevant once
    inlined — the browser never sees an `@import` rule, just the imported
    file's contents spliced in where the directive was."""
    path = path.resolve()
    if seen is None:
        seen = set()
    if path in seen:
        return ""
    seen.add(path)

    def repl(m):
        return resolve_css_imports(path.parent / m.group(1), seen)

    return CSS_IMPORT_RE.sub(repl, path.read_text())


def collapse_blank_lines(text: str) -> str:
    # Blank a whitespace-only line first (e.g. the indentation left behind where
    # a <script data-inline> tag was removed) so the bundle carries no trailing
    # whitespace, then collapse runs of blank lines. Only fully-blank lines are
    # touched, so trailing spaces never get stripped from real content.
    text = re.sub(r"(?m)^[ \t]+$", "", text)
    return re.sub(r"\n[ \t]*\n[ \t]*\n+", "\n\n", text)


def inline_html(src_html: Path) -> str:
    text = src_html.read_text()
    rel = src_html.relative_to(SRC_DIR).as_posix()

    text = text.replace(VERSION_TOKEN, FIRMWARE_VERSION)

    text = HTML_COMMENT_RE.sub("", text)

    def replace_link(m):
        attrs = parse_attrs(m.group("attrs"))
        if "data-inline" not in attrs:
            return m.group(0)
        href = attrs.get("href")
        if not href:
            raise ValueError(f"{rel}: <link data-inline> missing href")
        f = (src_html.parent / href).resolve()
        css = strip_css_comments(resolve_css_imports(f))
        return f"<style>\n{css}</style>"

    # Collect all <script data-inline> bodies in document order, then
    # concatenate them into one IIFE-wrapped <script>. This keeps every
    # module's top-level let/const/function out of the page's global
    # scope — the bundle sees them, the rest of the page does not.
    bodies = []
    for m in SCRIPT_TAG_RE.finditer(text):
        attrs = parse_attrs(m.group("attrs"))
        if "data-inline" not in attrs:
            continue
        src = attrs.get("src")
        if not src:
            raise ValueError(f"{rel}: <script data-inline> missing src")
        f = (src_html.parent / src).resolve()
        bodies.append((src, PROJECT_JS_SPDX_RE.sub("", f.read_text())))

    if bodies:
        chunks = [f"// --- {src} ---\n{body.rstrip()}" for src, body in bodies]
        iife = (
            "<script>\n"
            ";(function(){\n"
            + "\n".join(chunks)
            + "\n})();\n"
            "</script>"
        )
    else:
        iife = ""

    inserted = [False]

    def replace_script(m):
        attrs = parse_attrs(m.group("attrs"))
        if "data-inline" not in attrs:
            return m.group(0)
        if not inserted[0]:
            inserted[0] = True
            return iife
        return ""

    text = LINK_TAG_RE.sub(replace_link, text)
    text = SCRIPT_TAG_RE.sub(replace_script, text)
    text = collapse_blank_lines(text)

    # The year is the aggregate page's initial publication year, not the build
    # year, so it is a literal rather than today's date. Deriving it from the
    # clock would also make output depend on when a build ran, which
    # compute_hash() does not track and reproducible packaging relies on.
    banner = (
        f"<!-- Generated by scripts/build_web.py from web-src/{rel}. "
        f"Do not edit. -->\n"
        "<!-- SPDX-FileCopyrightText: 2026 ludable -->\n"
        "<!-- SPDX-License-Identifier: AGPL-3.0-only -->\n"
    )
    m = DOCTYPE_RE.search(text)
    if m:
        end = m.end()
        return text[:end] + "\n" + banner + text[end:].lstrip("\n")
    return banner + text


def gather_sources():
    if not SRC_DIR.is_dir():
        return []
    return sorted(p for p in SRC_DIR.rglob("*") if p.is_file())


def compute_hash(sources) -> str:
    h = hashlib.sha256()
    # Include this script's own bytes so edits to the inliner invalidate
    # the stamp even when web-src/ is unchanged.
    h.update(SELF.read_bytes())
    h.update(b"\0")
    h.update(FIRMWARE_VERSION.encode("utf-8"))
    h.update(b"\0")
    for p in sources:
        h.update(p.relative_to(SRC_DIR).as_posix().encode("utf-8"))
        h.update(b"\0")
        h.update(p.read_bytes())
        h.update(b"\0")
    return h.hexdigest()


def build() -> tuple[int, int]:
    pages = sorted(SRC_DIR.rglob("index.html"))
    expected = set()
    for src in pages:
        rel = src.relative_to(SRC_DIR)
        out = OUT_DIR / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(inline_html(src))
        expected.add(out.resolve())

    # Remove generated index.html files whose source has been deleted or
    # renamed; otherwise embed_web.py would keep embedding stale pages.
    removed = 0
    if OUT_DIR.is_dir():
        for stale in OUT_DIR.rglob("index.html"):
            if stale.resolve() not in expected:
                stale.unlink()
                removed += 1
    return len(pages), removed


def main() -> None:
    if not SRC_DIR.is_dir():
        return
    sources = gather_sources()
    current = compute_hash(sources)
    if (
        STAMP.exists()
        and STAMP.read_text().strip() == current
        and all((OUT_DIR / p.relative_to(SRC_DIR)).exists()
                for p in SRC_DIR.rglob("index.html"))
    ):
        return
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    n, removed = build()
    STAMP.write_text(current + "\n")
    suffix = f", removed {removed} stale page(s)" if removed else ""
    print(f"[build_web] inlined {n} page(s) from web-src/{suffix}")


main()
