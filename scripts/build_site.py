#!/usr/bin/env python3
"""Builds the GitHub Pages site for the UML diagrams.

    scripts/build_site.py --svg-dir build/uml --out site [--sha SHA] [--date D]

Reads the rendered *.svg files from --svg-dir and writes the site to --out:

    index.html   the listing page, generated here
    style.css    copied from docs/site/
    uml/*.svg    the diagrams

`gen_*` diagrams are generated from the code (clang-uml); everything else is
hand-authored. Page text and styling live in docs/site/ (style.css,
descriptions.json) so they can be changed without touching this script.
Standard library only.
"""
import argparse
import datetime
import html
import json
import re
import shutil
import sys
from pathlib import Path

REPO_URL = "https://github.com/markjfisher/fujinet-nio"
SITE_SRC = Path(__file__).resolve().parent.parent / "docs" / "site"


def title_for(stem: str) -> str:
    """CoreIOArchitecture -> 'Core IO Architecture'; seq_atari_x -> 'Atari x'."""
    name = re.sub(r"^(gen|arch|seq)_", "", stem)
    if "_" in name:
        name = name.replace("_", " ")
    else:
        name = re.sub(r"(?<=[a-z])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", " ", name)
    return name[:1].upper() + name[1:]


def kind_for(stem: str) -> str:
    low = stem.lower()
    if "seq" in low or "sequence" in low or "flow" in low:
        return "sequence"
    return "structure"


def card(svg: Path, descriptions: dict) -> str:
    stem = svg.stem
    info = descriptions.get(stem, {})
    title = html.escape(info.get("title") or title_for(stem))
    desc = html.escape(info.get("description", ""))
    href = f"uml/{html.escape(svg.name)}"
    kind = kind_for(stem)
    return f"""      <a class="card" href="{href}">
        <span class="thumb"><img src="{href}" alt="" loading="lazy"></span>
        <span class="body">
          <span class="title">{title}</span>
          <span class="badge {kind}">{kind}</span>
          <span class="desc">{desc}</span>
          <span class="file">{html.escape(svg.name)}</span>
        </span>
      </a>"""


def section(heading: str, blurb: str, svgs: list, descriptions: dict) -> str:
    if not svgs:
        return ""
    cards = "\n".join(card(s, descriptions) for s in svgs)
    return f"""    <section>
      <h2>{html.escape(heading)}</h2>
      <p class="blurb">{html.escape(blurb)}</p>
      <div class="grid">
{cards}
      </div>
    </section>
"""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--svg-dir", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--sha", default="")
    ap.add_argument("--date", default=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d"))
    args = ap.parse_args()

    svgs = sorted(args.svg_dir.glob("*.svg"))
    if not svgs:
        print(f"no .svg files in {args.svg_dir}", file=sys.stderr)
        return 1

    descriptions = {}
    desc_file = SITE_SRC / "descriptions.json"
    if desc_file.exists():
        descriptions = json.loads(desc_file.read_text(encoding="utf-8"))

    generated = [s for s in svgs if s.stem.startswith("gen_")]
    hand = [s for s in svgs if not s.stem.startswith("gen_")]

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "uml").mkdir(exist_ok=True)
    for s in svgs:
        shutil.copyfile(s, args.out / "uml" / s.name)
    shutil.copyfile(SITE_SRC / "style.css", args.out / "style.css")

    sha = args.sha.strip()
    built = f"Built {html.escape(args.date)}"
    if sha:
        built += f' from <a href="{REPO_URL}/commit/{html.escape(sha)}"><code>{html.escape(sha[:8])}</code></a>'

    page = f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>fujinet-nio diagrams</title>
  <link rel="stylesheet" href="style.css">
</head>
<body>
  <header>
    <div class="wrap">
      <h1>fujinet-nio <span>diagrams</span></h1>
      <p class="sub">Architecture and code structure of the FujiNet network adapter firmware.
        <a href="{REPO_URL}">Repository</a> &middot;
        <a href="{REPO_URL}/blob/master/docs/architecture.md">Architecture document</a></p>
    </div>
  </header>
  <main class="wrap">
{section("Architecture", "Hand-authored: the layering, protocols and flows the code is meant to follow.", hand, descriptions)}{section("Generated from code", "Produced by clang-uml on every push to master whose build and tests pass.", generated, descriptions)}  </main>
  <footer>
    <div class="wrap">{built}</div>
  </footer>
</body>
</html>
"""
    (args.out / "index.html").write_text(page, encoding="utf-8")
    print(f"wrote {args.out}/index.html ({len(hand)} hand-authored, {len(generated)} generated)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
