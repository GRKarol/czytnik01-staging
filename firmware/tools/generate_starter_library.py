#!/usr/bin/env python3
"""Builds the starter library shipped as GitHub Release assets.

Kreator pierwszego uruchomienia (ekran ladowania) pobiera 6 tytulow domeny
publicznej dla jezyka wybranego w kroku 1, patrz
App::bookDownloadTask (firmware/src/app/App.cpp). Ten skrypt generuje te
assety: pobiera zrodlowy plik (EPUB lub plain-text UTF-8) dla kazdej pozycji
w MANIFEST, konwertuje go do formatu .rsvp uzywajac tej samej logiki co
firmware/tools/sd_card_converter/convert_books.py (RsvpWriter, ekstrakcja
zdarzen z EPUB/HTML/tekstu), i zapisuje jako
"starter-<kod jezyka>-<1..6>.rsvp" w katalogu wyjsciowym.

Rozszerzenie MUSI byc .rsvp — StorageManager na urzadzeniu czyta dyrektywy
@title/@author/@chapter tylko z plikow .rsvp (patrz hasRsvpExtension() w
firmware/src/storage/StorageManager.cpp).

Uzycie (jak w build-fonts.yml/generate_font_pack.sh):
    python3 firmware/tools/generate_starter_library.py dist/books
"""

from __future__ import annotations

import html
import json
import pathlib
import re
import sys
import tempfile
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request

# Book titles/authors contain non-ASCII characters (accents, Polish/Romanian
# diacritics). CI runners default to UTF-8 stdout, but a local Windows
# console often doesn't — reconfigure so progress prints never crash the
# whole run over a console-encoding mismatch.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent / "sd_card_converter"))
import convert_books as cb  # noqa: E402  (path must be set up first)

MAX_WORDS = 0  # unlimited, same default as convert_books.py
# Bump with every change that alters the books; must match
# kStarterBookEdition in firmware/src/app/App.cpp. 2 = 17.10.2026: full
# Wolne Lektury texts, no donation/footnote chapters, books without
# chapters replaced.
STARTER_EDITION = 2
USER_AGENT = (
    "Mozilla/5.0 (compatible; czytnik01-starter-library-builder/1.0; "
    "+https://github.com/GRKarol/czytnik01)"
)
REQUEST_TIMEOUT_S = 90
RETRY_COUNT = 5
RETRY_DELAY_S = 4

GUTENBERG_START_RE = re.compile(
    r"\*\*\*\s*START OF (THE|THIS) PROJECT GUTENBERG EBOOK.*?\*\*\*", re.IGNORECASE | re.DOTALL
)
GUTENBERG_END_RE = re.compile(
    r"\*\*\*\s*END OF (THE|THIS) PROJECT GUTENBERG EBOOK", re.IGNORECASE
)

# Per-language chapter-heading keywords for plain-text (.txt) sources, mirroring
# the on-device keyword list StorageManager uses for un-annotated .txt books
# (firmware/src/storage/StorageManager.cpp) so generated chapters line up with
# what a human would expect to see for that language. Only used for the .txt
# fallback path — EPUB sources get real chapter markers from HTML headings.
CHAPTER_KEYWORDS = {
    "en": ("chapter", "part", "book"),
    "pl": ("rozdzial", "czesc"),
    "de": ("kapitel", "teil"),
    "fr": ("chapitre", "partie"),
    "es": ("capitulo",),
    "ro": ("capitolul", "partea"),
}


def strip_diacritics(text: str) -> str:
    decomposed = unicodedata.normalize("NFKD", text)
    return "".join(ch for ch in decomposed if not unicodedata.combining(ch))


def looks_like_chapter_lang(line: str, lang: str) -> str | None:
    trimmed = cb.clean_text(line)
    if not trimmed or len(trimmed) > 64:
        return None
    if trimmed.startswith("#"):
        title = trimmed.lstrip("#").strip()
        return title or None
    keywords = CHAPTER_KEYWORDS.get(lang, CHAPTER_KEYWORDS["en"])
    ascii_trimmed = strip_diacritics(trimmed).lower()
    for keyword in keywords:
        if re.match(rf"^{re.escape(keyword)}\b", ascii_trimmed):
            return trimmed
    return None


def text_events_lang(text: str, lang: str) -> list[tuple[str, str]]:
    events: list[tuple[str, str]] = []
    paragraph_parts: list[str] = []

    def flush_paragraph() -> None:
        if paragraph_parts:
            events.append(("text", cb.clean_text(" ".join(paragraph_parts))))
            paragraph_parts.clear()

    for raw_line in text.splitlines():
        line = raw_line.strip()
        chapter = looks_like_chapter_lang(line, lang)
        if chapter:
            flush_paragraph()
            events.append(("chapter", chapter))
            continue
        if not line:
            flush_paragraph()
            continue
        paragraph_parts.append(line)

    flush_paragraph()
    return events


def drop_empty_chapter_runs(events: list[tuple[str, str]]) -> list[tuple[str, str]]:
    """Collapses back-to-back chapter markers with no text between them to
    just the last one. Plain-text Gutenberg sources often print a table of
    contents ("Chapter I. The Cyclone" / "Chapter II. ..." / ...) ahead of the
    real story; looks_like_chapter_lang() matches each TOC line the same way
    it matches the real heading later, producing a run of empty chapters."""
    result: list[tuple[str, str]] = []
    for event in events:
        if (
            event[0] == "chapter"
            and result
            and result[-1][0] == "chapter"
        ):
            result[-1] = event
        else:
            result.append(event)
    return result


def strip_gutenberg_boilerplate(text: str) -> str:
    start_match = GUTENBERG_START_RE.search(text)
    if start_match:
        text = text[start_match.end():]
    end_match = GUTENBERG_END_RE.search(text)
    if end_match:
        text = text[: end_match.start()]
    return text.strip()


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    last_error: Exception | None = None
    for attempt in range(1, RETRY_COUNT + 1):
        try:
            with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT_S) as response:
                return response.read()
        except (urllib.error.URLError, TimeoutError) as exc:
            last_error = exc
            print(f"  fetch attempt {attempt}/{RETRY_COUNT} failed: {exc}")
            if attempt < RETRY_COUNT:
                time.sleep(RETRY_DELAY_S)
    raise RuntimeError(f"could not fetch {url}: {last_error}")


# Gutenberg wraps every EPUB (regardless of the book's own language) in the
# same English legal boilerplate as separate spine documents: title page,
# table of contents, "produced by"/transcriber notes, and a huge license
# block at the end. events_for_file()/epub_events_and_metadata() turn each
# into its own ("chapter", ...) section, which would otherwise show up as
# junk chapters ahead of/after the real story. Marker text is always English,
# so this is safe to apply to sources in any of the 6 UI languages.
BOILERPLATE_HEADING_MARKERS = (
    "gutenberg",
    "license",
    "transcriber",
    "produced by",
    "illustrations",
    "colophon",
    "title page",
    "frontispiece",
    "table of contents",
    "contents",
    "preface",
    "introduction",
    # Tables of contents, notes and errata in the other five languages.
    "table des mati",
    "inhalt",
    "índice",
    "indice",
    "spis treści",
    "treść",
    "cuprins",
    "footnote",
    "notas",
    "anmerkung",
    "transkription",
    "transcriptor",
    "transcripteur",
    "errata",
    "druckfehler",
    "objaśnienia",
    # Dedications.
    "dedicație",
    "dedykacja",
    "widmung",
    "dédicace",
    "dedicatoria",
    "dedication",
)
# A table of contents printed as plain paragraphs: many short lines. Only
# short sections qualify, so a long run of verse is never taken for one.
TOC_MIN_PARAGRAPHS = 4
TOC_MAX_AVERAGE_WORDS = 8
TOC_MAX_WORDS = 1500
BOILERPLATE_MIN_WORDS = 60
# Some editions front-load a long run of short sections before the real text
# starts (title page + dedication + a table-of-contents block where every
# entry is its own short HTML heading, e.g. Pan Tadeusz's ~14-section front
# matter) — cap generous enough to clear those, while _section_is_boilerplate's
# word-count/heading checks still stop at the first substantial section, so a
# book that's genuinely short throughout (a poetry collection) isn't gutted.
BOILERPLATE_MAX_TRIM_SECTIONS = 20
# Before the first real chapter a section this short is a title page, a
# dedication ("An Adelbert von Chamisso.", "Par Maurice Leblanc") or a
# licence note ("TASA" in Don Quijote), never a chapter of its own.
FRONT_MATTER_MAX_WORDS = 150


def _split_into_sections(
    events: list[tuple[str, str]]
) -> list[tuple[str | None, list[str]]]:
    sections: list[tuple[str | None, list[str]]] = []
    current_title: str | None = None
    current_texts: list[str] = []
    for kind, value in events:
        if kind == "chapter":
            sections.append((current_title, current_texts))
            current_title = value
            current_texts = []
        else:
            current_texts.append(value)
    sections.append((current_title, current_texts))
    return [s for s in sections if s[0] is not None or s[1]]


# Headings that are boilerplate only as the whole heading ("Note" at the
# end of a Romanian edition, not "Notebook").
BOILERPLATE_EXACT_HEADINGS = ("note", "notes", "fin", "sfârșit", "ende")


def _section_is_boilerplate(title: str | None, texts: list[str]) -> bool:
    if title is not None:
        lowered = title.lower()
        if any(marker in lowered for marker in BOILERPLATE_HEADING_MARKERS):
            return True
        if lowered.strip(" .") in BOILERPLATE_EXACT_HEADINGS:
            return True
    word_count = sum(len(t.split()) for t in texts)
    if word_count < BOILERPLATE_MIN_WORDS:
        return True
    # Short lines alone are verse too (the Epilog of Pan Tadeusz went as a
    # "table of contents"); a printed contents list also ends its lines in
    # page numbers.
    numbered = sum(1 for t in texts if re.search(r"\d+\s*\.?$", t.strip()))
    return (
        len(texts) >= TOC_MIN_PARAGRAPHS
        and word_count <= TOC_MAX_WORDS
        and word_count / len(texts) < TOC_MAX_AVERAGE_WORDS
        and numbered * 10 >= len(texts) * 3
    )


def trim_boilerplate_sections(events: list[tuple[str, str]]) -> list[tuple[str, str]]:
    sections = _split_into_sections(events)
    if len(sections) <= 1:
        return events

    start = 0
    while (
        start < len(sections) - 1
        and start < BOILERPLATE_MAX_TRIM_SECTIONS
        and (
            _section_is_boilerplate(*sections[start])
            or sum(len(t.split()) for t in sections[start][1]) < FRONT_MATTER_MAX_WORDS
        )
    ):
        start += 1

    end = len(sections)
    trimmed_from_end = 0
    while (
        end > start + 1
        and trimmed_from_end < BOILERPLATE_MAX_TRIM_SECTIONS
        and _section_is_boilerplate(*sections[end - 1])
    ):
        end -= 1
        trimmed_from_end += 1

    kept_events: list[tuple[str, str]] = []
    for title, texts in sections[start:end]:
        if title is not None:
            kept_events.append(("chapter", title))
        for text in texts:
            kept_events.append(("text", text))
    return kept_events


# Some Gutenberg epubs append the license footer to the SAME spine file as
# the final chapter (no separate heading), so section-level trimming above
# can't isolate it. This phrase essentially never occurs in 19th/20th-century
# prose, so treat its first appearance as "everything from here is boilerplate"
# and cut the stream there.
BOILERPLATE_INLINE_MARKERS = (
    "project gutenberg",
    "gutenberg-tm",
    "gutenberg™",
    # Wolne Lektury's closing note (cover credits, ISBN, file date).
    "wolnelektury.pl",
    "fundacja nowoczesna polska",
    "isbn-978-83-288",
)


# Wolne Lektury put a donation appeal ("fund0", "fund1", ... every few
# chapters), the footnotes, their own closing page ("last") and a
# "Wesprzyj Wolne Lektury!" page into the spine as sections of their own.
# They go whole, before the inline cut below: the first appeal holds a
# wolnelektury.pl link, and cutting there kept only the first 4 chapters
# of "W pustyni i w puszczy" (Karol 17.10).
WOLNE_LEKTURY_SECTION_RE = re.compile(r"^(fund\d+|last|przypisy:?|wesprzyj wolne lektury!?)$", re.IGNORECASE)


def drop_wolne_lektury_sections(events: list[tuple[str, str]]) -> list[tuple[str, str]]:
    result: list[tuple[str, str]] = []
    skipping = False
    for kind, value in events:
        if kind == "chapter":
            skipping = bool(WOLNE_LEKTURY_SECTION_RE.match(value.strip()))
            if skipping:
                continue
        if not skipping:
            result.append((kind, value))
    return result


def truncate_at_first_inline_boilerplate(
    events: list[tuple[str, str]]
) -> list[tuple[str, str]]:
    result: list[tuple[str, str]] = []
    for kind, value in events:
        if kind == "text" and any(m in value.lower() for m in BOILERPLATE_INLINE_MARKERS):
            break
        result.append((kind, value))
    return result


# Wikisource (Romanian: Project Gutenberg has only 3 usable Romanian books).
# The MediaWiki parse API gives each page's rendered HTML; the header box,
# edit links, footnotes and other page furniture are dropped before the
# same HTML extractor as EPUB chapters reads it. A page that is just a table
# of contents (the novel split into subpages) is followed one level down,
# in the order the contents list them, each subpage becoming a chapter.
WIKISOURCE_SKIP_CLASSES = (
    "ws-noexport", "noprint", "mw-editsection", "reference", "references",
    "mw-references-wrap", "toc", "navbox", "wst-header", "headertemplate",
    "mw-empty-elt", "plainSister",
)
WIKISOURCE_VOID_TAGS = {"br", "img", "hr", "meta", "link", "input", "wbr", "source", "col", "area"}
WIKISOURCE_MIN_WORDS_OWN_TEXT = 1500


class _WikisourceCleaner(cb.HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.out: list[str] = []
        self.skip_depth = 0
        self.links: list[str] = []

    def _skipped(self, tag: str, attrs) -> bool:
        if tag in ("style", "script", "sup", "table"):
            return True
        classes = dict(attrs).get("class") or ""
        return any(name in classes for name in WIKISOURCE_SKIP_CLASSES)

    def handle_starttag(self, tag, attrs):
        if tag in WIKISOURCE_VOID_TAGS:
            if not self.skip_depth:
                self.out.append(f"<{tag}>")
            return
        if self.skip_depth:
            self.skip_depth += 1
            return
        if self._skipped(tag, attrs):
            self.skip_depth = 1
            return
        if tag == "a":
            href = dict(attrs).get("href") or ""
            if href.startswith("/wiki/"):
                self.links.append(urllib.parse.unquote(href[len("/wiki/"):]).replace("_", " "))
        self.out.append(f"<{tag}>")

    def handle_endtag(self, tag):
        if tag in WIKISOURCE_VOID_TAGS:
            return
        if self.skip_depth:
            self.skip_depth -= 1
            return
        self.out.append(f"</{tag}>")

    def handle_data(self, data):
        if not self.skip_depth:
            self.out.append(html.escape(data, quote=False))


def _wikisource_page(lang: str, title: str) -> tuple[str, str, list[str]]:
    query = urllib.parse.urlencode({
        "action": "parse", "page": title, "prop": "text", "redirects": 1,
        "disableeditsection": 1, "disabletoc": 1, "format": "json", "formatversion": 2,
    })
    data = json.loads(fetch(f"https://{lang}.wikisource.org/w/api.php?{query}").decode("utf-8"))
    if "error" in data:
        raise RuntimeError(f"wikisource {title}: {data['error'].get('info')}")
    cleaner = _WikisourceCleaner()
    cleaner.feed(data["parse"]["text"])
    return data["parse"]["title"], "".join(cleaner.out), cleaner.links


def wikisource_events(lang: str, title: str) -> list[tuple[str, str]]:
    resolved, markup, links = _wikisource_page(lang, title)
    events = [e for e in cb.html_events(markup) if e[0] == "chapter" or e[1].strip()]
    own_words = sum(len(v.split()) for k, v in events if k == "text")
    subpages: list[str] = []
    for link in links:
        if link.startswith(resolved + "/") and link not in subpages:
            subpages.append(link)
    if own_words >= WIKISOURCE_MIN_WORDS_OWN_TEXT or not subpages:
        return events
    result: list[tuple[str, str]] = []
    for page in subpages:
        _, sub_markup, _ = _wikisource_page(lang, page)
        result.append(("chapter", page[len(resolved) + 1:]))
        result.extend(e for e in cb.html_events(sub_markup) if e[0] == "chapter" or e[1].strip())
        time.sleep(0.5)
    return result


FOOTNOTE_MARK_RE = re.compile(r"\s*\[\d{1,4}\]")
FOOTNOTE_BODY_RE = re.compile(r"^\[\d{1,4}\]\s")
# Chapter names Gutenberg's EPUB builder leaves for files without a heading
# ("6345988651784513070 74008 h 1.htm").
FILE_CHAPTER_RE = re.compile(r"^\d{6,} \d+ h \d+\.htm$|\.x?html?$")


# Wolne Lektury footnote anchors come out glued to the word they mark
# ("Saidu1", "emirem4", chapter "Zaścianek370").
GLUED_FOOTNOTE_RE = re.compile(r"(?<=[^\W\d_])\d{1,4}(?=[\s.,;:!?…\"”'»)\-—]|$)")


def polish_events(events: list[tuple[str, str]], glued_footnotes: bool = False) -> list[tuple[str, str]]:
    """Drops footnote bodies and markers and file-name chapter titles: in a
    one-word-at-a-time reader "[192]" and "Giftkraut." between two sentences
    are just noise."""
    result: list[tuple[str, str]] = []
    for kind, value in events:
        if kind == "chapter":
            if FILE_CHAPTER_RE.search(value.strip()):
                continue
            value = GLUED_FOOTNOTE_RE.sub("", FOOTNOTE_MARK_RE.sub("", value).strip())
            # The converter writes a middle dot as "*"; in a chapter name
            # ("ZWEITES KAPITEL · ERSTE UNTERSUCHUNG") a dash reads better.
            value = re.sub(r"\s*[·•∙]\s*", " - ", value)
            result.append((kind, value))
            continue
        if FOOTNOTE_BODY_RE.match(value):
            continue
        value = FOOTNOTE_MARK_RE.sub("", value).strip()
        if glued_footnotes:
            value = GLUED_FOOTNOTE_RE.sub("", value)
        if value:
            result.append((kind, value))
    return result


MAX_CHAPTER_TITLE_CHARS = 60
# A "chapter" this short is a part heading with a page number or a picture
# caption under it ("PART TWO-The Sea-cook" in Treasure Island): it goes
# into the chapter after it instead of being a stop of its own.
MIN_CHAPTER_WORDS = 25


def _joined_title(first: str, second: str) -> str:
    separator = " " if second.startswith("(") else " - "
    joined = f"{first}{separator}{second}"
    return joined if len(joined) <= MAX_CHAPTER_TITLE_CHARS else second


def tidy_chapter_structure(events: list[tuple[str, str]], lang: str = "en") -> list[tuple[str, str]]:
    """Every chapter the reader lists must be one a person would pick: a
    heading with its subtitle ("Głucha przestrzeń (Ballada kolejowa)"), no
    empty part headings, no end markers ("FIN DE CANDIDE.")."""
    sections = _split_into_sections(events)
    merged: list[tuple[str | None, list[str]]] = []
    carry_title: str | None = None
    carry_texts: list[str] = []
    for title, texts in sections:
        if carry_title is not None:
            title = _joined_title(carry_title, title) if title is not None else carry_title
            texts = carry_texts + texts
            carry_title = None
            carry_texts = []
        words = sum(len(t.split()) for t in texts)
        if title is not None and words < MIN_CHAPTER_WORDS:
            carry_title = title
            carry_texts = texts
            continue
        merged.append((title, texts))
    # A short chapter left at the very end is an end marker: its few words
    # go to the chapter before it.
    if carry_title is not None and merged:
        last_title, last_texts = merged[-1]
        merged[-1] = (last_title, last_texts + carry_texts)

    merged = number_restarting_parts(merged, lang)

    result: list[tuple[str, str]] = []
    for title, texts in merged:
        if title is not None:
            result.append(("chapter", title))
        result.extend(("text", t) for t in texts)
    return result


ROMAN_RE = re.compile(r"^[IVXLC]+\.?$")
PART_WORD = {"pl": "Tom", "en": "Part", "de": "Teil", "fr": "Partie", "es": "Parte", "ro": "Partea"}


def number_restarting_parts(
    sections: list[tuple[str | None, list[str]]], lang: str
) -> list[tuple[str | None, list[str]]]:
    """A book in volumes whose chapters start again at "I" in each one
    ("Stara baśń": I-XI, I-X, I-IX, the volume headings lost in the EPUB)
    gets the volume in front: "Tom 2 - III"."""
    titles = [t for t, _ in sections if t is not None]
    restarts = sum(
        1 for prev, cur in zip(titles, titles[1:]) if cur.rstrip(".") == "I" and ROMAN_RE.match(prev)
    )
    if restarts == 0:
        return sections
    part = 1
    previous: str | None = None
    numbered: list[tuple[str | None, list[str]]] = []
    for title, texts in sections:
        if title is not None and ROMAN_RE.match(title):
            if title.rstrip(".") == "I" and previous is not None and ROMAN_RE.match(previous):
                part += 1
            previous = title
            title = f"{PART_WORD.get(lang, 'Part')} {part} - {title.rstrip('.')}"
        elif title is not None:
            previous = title
        numbered.append((title, texts))
    return numbered


def apply_entry_fixes(events: list[tuple[str, str]], entry: dict) -> list[tuple[str, str]]:
    """Hand fixes from the manifest for what no general rule catches:
    "drop_sections" (heading regexes, the heading goes with its text),
    "drop_leading_text" (a title page before the first heading),
    "drop_text" (exact lines, e.g. the alt text left by illustrations)."""
    drops = [re.compile(p, re.IGNORECASE) for p in entry.get("drop_sections", [])]
    drop_text = set(entry.get("drop_text", []))
    result: list[tuple[str, str]] = []
    skipping = bool(entry.get("drop_leading_text"))
    for kind, value in events:
        if kind == "chapter":
            skipping = any(d.search(value) for d in drops)
            if skipping:
                continue
        if kind == "text" and drop_text:
            if value.strip() in drop_text:
                continue
            first, _, rest = value.strip().partition(" ")
            if first in drop_text and rest:
                value = rest
        if not skipping:
            result.append((kind, value))
    return result


def rename_chapters(events: list[tuple[str, str]], entry: dict) -> list[tuple[str, str]]:
    renames = [(re.compile(p), r) for p, r in entry.get("rename", [])]
    if not renames:
        return events
    result: list[tuple[str, str]] = []
    for kind, value in events:
        if kind == "chapter":
            for pattern, replacement in renames:
                value = pattern.sub(replacement, value)
        result.append((kind, value))
    return result


def build_one(entry: dict, tmp_dir: pathlib.Path, out_dir: pathlib.Path) -> None:
    lang = entry["lang"]
    slot = entry["slot"]
    url = entry["url"]
    kind = entry["kind"]
    title = entry["title"]
    author = entry["author"]

    print(f"[{lang}-{slot}] {title} / {author} <- {url}")
    data = fetch(url) if kind != "wikisource" else b""

    if kind == "epub":
        src_path = tmp_dir / f"src-{lang}-{slot}.epub"
        src_path.write_bytes(data)
        _, extracted_author, events = cb.epub_events_and_metadata(src_path)
        author = author or extracted_author
        events = drop_wolne_lektury_sections(events)
        events = trim_boilerplate_sections(events)
        events = truncate_at_first_inline_boilerplate(events)
    elif kind == "wikisource":
        events = wikisource_events(lang, url)
        events = trim_boilerplate_sections(events)
        events = drop_empty_chapter_runs(events)
    elif kind == "txt":
        text = data.decode("utf-8-sig", errors="replace")
        text = strip_gutenberg_boilerplate(text)
        events = text_events_lang(text, lang)
        events = drop_empty_chapter_runs(events)
    else:
        raise ValueError(f"unknown source kind: {kind}")

    events = polish_events(events, glued_footnotes="wolnelektury.pl" in url)
    events = apply_entry_fixes(events, entry)
    events = tidy_chapter_structure(events, lang)
    events = rename_chapters(events, entry)
    writer = cb.RsvpWriter(title=title, author=author, source=url, max_words=MAX_WORDS)
    # The reader keeps a starter book already on the card unless its
    # edition is older than STARTER_EDITION (App::bookDownloadTask).
    writer.lines.insert(writer.lines.index(""), f"@edition {STARTER_EDITION}")
    for event_kind, value in events:
        if event_kind == "chapter":
            writer.add_chapter(value)
            continue
        writer.begin_paragraph()
        if not writer.add_text(value):
            break

    if writer.word_count < 200:
        raise RuntimeError(
            f"[{lang}-{slot}] suspiciously short output ({writer.word_count} words) — "
            "source likely didn't parse as expected"
        )

    out_path = out_dir / f"starter-{lang}-{slot}.rsvp"
    writer.write_to(out_path, fallback_chapter=title)
    print(
        f"  -> {out_path.name} "
        f"({writer.word_count} words, {writer.chapter_count} chapters)"
    )


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <output-dir>", file=sys.stderr)
        return 2

    out_dir = pathlib.Path(sys.argv[1]).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="starter-library-") as tmp:
        tmp_dir = pathlib.Path(tmp)
        for entry in MANIFEST:
            try:
                build_one(entry, tmp_dir, out_dir)
            except Exception as exc:  # noqa: BLE001 - report and continue, don't fail the whole pack
                label = f"{entry['lang']}-{entry['slot']}"
                failures.append(f"{label}: {exc}")
                print(f"[{label}] FAILED: {exc}")

    total = len(MANIFEST)
    ok = total - len(failures)
    print(f"\nBuilt {ok}/{total} starter books")
    if failures:
        print("Failures:")
        for line in failures:
            print(f"  - {line}")
    # A partial pack is still useful (missing assets are skipped silently by
    # the firmware, see startBackgroundBookDownload's comment in App.cpp) —
    # only fail the build if we produced dangerously few books overall.
    if ok < total // 2:
        return 1
    return 0


def _gutenberg(book_id: int) -> str:
    # .epub.noimages: same text content as the full epub, without cover/illustration
    # images we'd throw away anyway (RsvpWriter only extracts text) — much
    # smaller and faster to fetch/parse in CI.
    return f"https://www.gutenberg.org/ebooks/{book_id}.epub.noimages"


def _wolnelektury(slug: str) -> str:
    return f"https://wolnelektury.pl/media/book/epub/{slug}.epub"


# 6 public-domain books per UI language (see firmware/src/app/Localization.h),
# each written in that language, one per genre where the shelf allows:
# adventure, crime, romance, horror/fantastic, science fiction, humour or
# short stories. Sources: Project Gutenberg; Wolne Lektury for Polish;
# Romanian Wikisource for Romanian (Gutenberg has only 3 Romanian books).
# Every source was fetched and converted before being listed here.
MANIFEST: list[dict] = [
    # --- English (en) ---
    {"lang": "en", "slot": 1, "kind": "epub", "url": _gutenberg(1661),
     "title": "The Adventures of Sherlock Holmes", "author": "Arthur Conan Doyle"},
    {"lang": "en", "slot": 2, "kind": "epub", "url": _gutenberg(1342),
     "title": "Pride and Prejudice", "author": "Jane Austen"},
    {"lang": "en", "slot": 3, "kind": "epub", "url": _gutenberg(120),
     "title": "Treasure Island", "author": "Robert Louis Stevenson",
     # List of illustrations and the "To the hesitating purchaser" poem.
     "drop_sections": [r"^Illustrated by", r"^TREASURE ISLAND$"]},
    {"lang": "en", "slot": 4, "kind": "epub", "url": _gutenberg(345),
     "title": "Dracula", "author": "Bram Stoker"},
    {"lang": "en", "slot": 5, "kind": "epub", "url": _gutenberg(35),
     "title": "The Time Machine", "author": "H. G. Wells"},
    {"lang": "en", "slot": 6, "kind": "epub", "url": _gutenberg(308),
     "title": "Three Men in a Boat", "author": "Jerome K. Jerome"},

    # --- French (fr) ---
    {"lang": "fr", "slot": 1, "kind": "epub", "url": _gutenberg(32854),
     "title": "Arsène Lupin, gentleman-cambrioleur", "author": "Maurice Leblanc"},
    {"lang": "fr", "slot": 2, "kind": "epub", "url": _gutenberg(13951),
     "title": "Les trois mousquetaires", "author": "Alexandre Dumas"},
    {"lang": "fr", "slot": 3, "kind": "epub", "url": _gutenberg(4791),
     "title": "Voyage au centre de la Terre", "author": "Jules Verne"},
    {"lang": "fr", "slot": 4, "kind": "epub", "url": _gutenberg(62215),
     "title": "Le Fantôme de l'Opéra", "author": "Gaston Leroux"},
    {"lang": "fr", "slot": 5, "kind": "epub", "url": _gutenberg(2419),
     "title": "La dame aux camélias", "author": "Alexandre Dumas fils"},
    {"lang": "fr", "slot": 6, "kind": "epub", "url": _gutenberg(4650),
     "title": "Candide", "author": "Voltaire",
     # Title page and the 19th-century editor's preface, not Voltaire's.
     "drop_leading_text": True, "drop_sections": [r"^Préface de l'Éditeur"]},

    # --- German (de) ---
    {"lang": "de", "slot": 1, "kind": "epub", "url": _gutenberg(29336),
     "title": "Durch Wüste und Harem", "author": "Karl May"},
    {"lang": "de", "slot": 2, "kind": "epub", "url": _gutenberg(22367),
     "title": "Die Verwandlung", "author": "Franz Kafka"},
    {"lang": "de", "slot": 3, "kind": "epub", "url": _gutenberg(50285),
     "title": "Dr. Mabuse, der Spieler", "author": "Norbert Jacques"},
    # Der Schimmelreiter runs as one piece with no chapters (Karol 17.10).
    {"lang": "de", "slot": 4, "kind": "epub", "url": _gutenberg(17362),
     "title": "Der goldene Topf", "author": "E. T. A. Hoffmann"},
    {"lang": "de", "slot": 5, "kind": "epub", "url": _gutenberg(31538),
     "title": "Peter Schlemihls wundersame Geschichte", "author": "Adelbert von Chamisso",
     # A closing dedication, an "Ende." page and the publisher's list after
     # chapter 11; the bare title heading in front of chapter 1.
     "drop_sections": [r"^An Adelbert von Chamisso", r"^Ende", r"^Peter Schlemihls wundersame Geschichte"]},
    {"lang": "de", "slot": 6, "kind": "epub", "url": _gutenberg(77905),
     "title": "Deutsche Märchen", "author": "Brüder Grimm",
     # Every illustration left its alt text "deko" as a paragraph.
     "drop_text": ["deko"]},

    # --- Spanish (es) ---
    {"lang": "es", "slot": 1, "kind": "epub", "url": _gutenberg(2000),
     "title": "Don Quijote de la Mancha", "author": "Miguel de Cervantes",
     # The licences, errata and approvals printed in front of both parts.
     "drop_sections": [r"^por Miguel de Cervantes", r"TASA$", r"ERRATAS", r"^EL REY$",
                       r"^APROBACI", r"^DEDICATORIA", r"^AL DUQUE DE B"]},
    {"lang": "es", "slot": 2, "kind": "epub", "url": _gutenberg(13507),
     "title": "Cuentos de amor, de locura y de muerte", "author": "Horacio Quiroga"},
    {"lang": "es", "slot": 3, "kind": "epub", "url": _gutenberg(26983),
     "title": "Sangre y arena", "author": "Vicente Blasco Ibáñez"},
    {"lang": "es", "slot": 4, "kind": "epub", "url": _gutenberg(17223),
     "title": "Pepita Jiménez", "author": "Juan Valera"},
    {"lang": "es", "slot": 5, "kind": "epub", "url": _gutenberg(320),
     "title": "Lazarillo de Tormes", "author": "Anónimo"},
    {"lang": "es", "slot": 6, "kind": "epub", "url": _gutenberg(49836),
     "title": "Niebla", "author": "Miguel de Unamuno"},

    # --- Romanian (ro) ---
    {"lang": "ro", "slot": 1, "kind": "wikisource", "url": "Amintiri din copilărie",
     "title": "Amintiri din copilărie", "author": "Ion Creangă"},
    {"lang": "ro", "slot": 2, "kind": "wikisource", "url": "Moara cu noroc",
     "title": "Moara cu noroc", "author": "Ioan Slavici"},
    # Harap-Alb and Sărmanul Dionis have no chapters at all: the reader's
    # chapter list stayed one entry long (Karol 17.10).
    {"lang": "ro", "slot": 3, "kind": "wikisource", "url": "Mara",
     "title": "Mara", "author": "Ioan Slavici"},
    {"lang": "ro", "slot": 4, "kind": "wikisource", "url": "Ciocoii vechi și noi",
     "title": "Ciocoii vechi și noi", "author": "Nicolae Filimon"},
    {"lang": "ro", "slot": 5, "kind": "wikisource", "url": "Ion (Rebreanu)",
     "title": "Ion", "author": "Liviu Rebreanu"},
    {"lang": "ro", "slot": 6, "kind": "epub", "url": _gutenberg(64597),
     "title": "Nuvele", "author": "Ion Luca Caragiale"},

    # --- Polish (pl) ---
    {"lang": "pl", "slot": 1, "kind": "epub", "url": _wolnelektury("w-pustyni-i-w-puszczy"),
     "title": "W pustyni i w puszczy", "author": "Henryk Sienkiewicz"},
    {"lang": "pl", "slot": 2, "kind": "epub", "url": _wolnelektury("dolega-mostowicz-prokurator-alicja-horn"),
     "title": "Prokurator Alicja Horn", "author": "Tadeusz Dołęga-Mostowicz"},
    # Na srebrnym globie: its first part is a 32 000-word diary with no
    # chapters, so it was swapped for a novel split all the way through.
    {"lang": "pl", "slot": 3, "kind": "epub", "url": _wolnelektury("stara-basn"),
     "title": "Stara baśń", "author": "Józef Ignacy Kraszewski"},
    {"lang": "pl", "slot": 4, "kind": "epub", "url": _wolnelektury("demon-ruchu"),
     "title": "Demon ruchu", "author": "Stefan Grabiński"},
    # Wolne Lektury rather than Gutenberg 31536, which has the first five
    # books only, with page numbers and its contents list in the text.
    {"lang": "pl", "slot": 5, "kind": "epub", "url": _wolnelektury("pan-tadeusz"),
     "title": "Pan Tadeusz", "author": "Adam Mickiewicz"},
    {"lang": "pl", "slot": 6, "kind": "epub", "url": _gutenberg(8119),
     "title": "Sklepy cynamonowe", "author": "Bruno Schulz"},
]


if __name__ == "__main__":
    raise SystemExit(main())
