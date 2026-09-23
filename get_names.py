#!/usr/bin/env python3
"""
Creature Card updater:
  1. Reads every *.nfc file in a folder
  2. Extracts the creature ID (page 12, bytes 0-1 -> lowercase hex)
  3. Fetches https://cc.mysteryhack.com/creature/<ID>
  4. Locates the <object class="creature-card"> tag:
       - if it references an external SVG (data=/archive=), fetches that SVG
       - if the SVG is inline, uses the page HTML directly
  5. Extracts the creature name from <g id="name"><text>NAME</text></g>
  6. Renames the .nfc to <Name>.nfc (original kept as .bak)
Files without an ID, name, or failed fetches are skipped.
"""

import os
import re
import sys
import html
import urllib.request
import urllib.error
from urllib.parse import urlparse, urljoin

BASE = "https://cc.mysteryhack.com/creature/"
ID_OFFSET_PAGE = 12            # creature ID is on page 12, bytes 0-1
KEEP_ORIGINAL = True           # keep original file as <id>.nfc.bak
RENAME_NFC = True              # rename .nfc to creature name

UA = {"User-Agent": "Mozilla/5.0 (creature-card-updater)"}

# --------------------------------------------------------------- parsing .nfc

def parse_pages(path):
    pages = {}
    try:
        text = open(path, "r", encoding="utf-8", errors="replace").read()
    except OSError:
        return None
    for line in text.splitlines():
        m = re.match(r"\s*Page\s+(\d+):\s*([0-9A-Fa-f ]+)\s*$", line)
        if m:
            try:
                pages[int(m.group(1))] = bytes(int(b, 16) for b in m.group(2).split())
            except ValueError:
                pass
    return pages

def extract_creature_id(pages):
    if 12 not in pages or len(pages[12]) < 2:
        return None
    return pages[12][:2].hex()

# --------------------------------------------------------------- web helpers

def fetch(url, binary=False):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=15) as r:
        data = r.read()
    return data if binary else data.decode("utf-8", errors="replace")

# --------------------------------------------------------------- SVG name extraction

def strip_tags(s):
    """Remove nested tags inside <text> (e.g. <tspan>), unescape entities."""
    s = re.sub(r"<[^>]+>", "", s)
    return html.unescape(s).strip()

def extract_name_from_svg(svg):
    """Find <g id="name"> ... <text ...>NAME</text> in an SVG string."""
    # locate the g element with id="name" (attribute order may vary)
    g = re.search(
        r'<g\b[^>]*\bid\s*=\s*["\']name["\'][^>]*>(.*?)(?=<g\b[^>]*\bid\s*=\s*["\']|</svg>)',
        svg, re.I | re.S)
    scope = g.group(1) if g else svg
    # first <text> element inside that group
    t = re.search(r"<text\b[^>]*>(.*?)</text>", scope, re.I | re.S)
    if t:
        name = strip_tags(t.group(1))
        if name:
            return name
    # fallback: any <text> with non-empty content in the whole svg
    for t in re.finditer(r"<text\b[^>]*>(.*?)</text>", svg, re.I | re.S):
        name = strip_tags(t.group(1))
        if name:
            return name
    return None

def get_creature_svg_url(page_html, page_url):
    """Find <object class="creature-card" ... data="..."> or <embed src="...">."""
    for m in re.finditer(
            r'<(?:object|embed)\b[^>]*class\s*=\s*["\'][^"\']*creature-card[^"\']*["\'][^>]*>',
            page_html, re.I):
        tag = m.group(0)
        u = re.search(r'\b(?:data|src)\s*=\s*["\']([^"\']+)["\']', tag, re.I)
        if u:
            return urljoin(page_url, html.unescape(u.group(1)))
    # attribute order variant: data before class
    for m in re.finditer(
            r'<(?:object|embed)\b[^>]*\b(?:data|src)\s*=\s*["\']([^"\']+)["\'][^>]*>',
            page_html, re.I):
        tag = m.group(0)
        if re.search(r'class\s*=\s*["\'][^"\']*creature-card', tag, re.I):
            return urljoin(page_url, html.unescape(m.group(1)))
    return None

# --------------------------------------------------------------- main

def safe_filename(name):
    return re.sub(r"[^A-Za-z0-9_\- ]", "", name).strip().replace(" ", "_") or "creature"

def process(path):
    pages = parse_pages(path)
    if not pages:
        return None, "no pages parsed"
    cid = extract_creature_id(pages)
    if not cid:
        return None, "no creature ID"

    url = BASE + cid
    try:
        page_html = fetch(url)
    except (urllib.error.URLError, urllib.error.HTTPError, OSError) as e:
        return cid, f"fetch page failed: {e}"

    # inline SVG on the page?
    name = None
    if re.search(r'<g\b[^>]*\bid\s*=\s*["\']name["\']', page_html, re.I):
        name = extract_name_from_svg(page_html)

    # otherwise fetch the external SVG referenced by the object tag
    if not name:
        svg_url = get_creature_svg_url(page_html, url)
        if not svg_url:
            return cid, "no .creature-card object / SVG found"
        try:
            svg = fetch(svg_url)
        except (urllib.error.URLError, urllib.error.HTTPError, OSError) as e:
            return cid, f"fetch svg failed ({svg_url}): {e}"
        name = extract_name_from_svg(svg)

    if not name:
        return cid, "no name found in SVG g#name"

    # rename the .nfc
    renamed = False
    new_path = path
    if RENAME_NFC:
        base_dir = os.path.dirname(path)
        new_path = os.path.join(base_dir, safe_filename(name) + ".nfc")
        if os.path.abspath(new_path) != os.path.abspath(path):
            if KEEP_ORIGINAL and not os.path.exists(path + ".bak"):
                os.replace(path, path + ".bak")
                os.replace(path + ".bak", new_path) if False else os.replace(path + ".bak", new_path)
            else:
                os.replace(path, new_path)
            renamed = True

    return cid, (name, new_path, renamed)

def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "."
    files = sorted(f for f in os.listdir(folder) if f.lower().endswith(".nfc"))

    ok = fail = 0
    for fname in files:
        path = os.path.join(folder, fname)
        cid, result = process(path)
        if isinstance(result, str):
            print(f"[FAIL] {fname} (id={cid}): {result}")
            fail += 1
            continue
        name, new_path, renamed = result
        print(f"[OK]   {fname} -> '{name}'  ({os.path.basename(new_path)})")
        ok += 1

    print(f"\nDone: {ok} renamed, {fail} skipped/failed.")

if __name__ == "__main__":
    main()