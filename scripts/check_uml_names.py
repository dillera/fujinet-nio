#!/usr/bin/env python3
"""Checks that the hand-authored UML diagrams only name things that exist.

    scripts/check_uml_names.py [--strict] [FILE.puml ...]

Hand-authored diagrams (docs/uml/arch_*.puml and seq_*.puml) drift when code is
renamed. This extracts the code-looking names from each diagram (CamelCase
types, snake_case or camelCase functions followed by "(", FN_* macros) and
reports any that no longer appear anywhere in include/, src/ or tests/.

It catches renamed or removed types and functions, not a wrong flow, so it is a
safety net and not a substitute for reading the diagram. Names that are not
code (products, external systems) go in scripts/uml_names_allow.txt.

Exit status is 0 unless --strict is given and something is missing. Output uses
GitHub Actions "::warning" annotations when GITHUB_ACTIONS is set.
Standard library only.
"""
import argparse
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CODE_DIRS = ["include", "src", "tests"]
CODE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp", ".cmake", ".txt", ".py", ".yml", ".yaml"}
ALLOW_FILE = Path(__file__).resolve().parent / "uml_names_allow.txt"

WORD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CAMEL = re.compile(r"\b(?:[A-Z][a-z0-9]+){2,}\b|\b[A-Z]{2,}(?:[A-Z][a-z0-9]+)+\b")
FUNC = re.compile(r"\b([a-z][a-z0-9]*(?:_[a-z0-9]+)+|[a-z]+(?:[A-Z][a-z0-9]*)+)\(")
MACRO = re.compile(r"\bFN_[A-Z0-9_]+\b")
ALIAS = re.compile(r"\bas\s+([A-Za-z_][A-Za-z0-9_]*)\b")


def code_words() -> set:
    words = set()
    for d in CODE_DIRS:
        base = ROOT / d
        if not base.exists():
            continue
        for dirpath, _dirs, files in os.walk(base):
            for name in files:
                if Path(name).suffix in CODE_SUFFIXES:
                    try:
                        text = (Path(dirpath) / name).read_text(encoding="utf-8", errors="ignore")
                    except OSError:
                        continue
                    words.update(WORD.findall(text))
                    words.add(Path(name).stem)  # file names, e.g. slip_codec
    return words


def allowed() -> set:
    if not ALLOW_FILE.exists():
        return set()
    out = set()
    for line in ALLOW_FILE.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.add(line)
    return out


def names_in(puml: Path) -> dict:
    """name -> first line number."""
    found = {}
    text = puml.read_text(encoding="utf-8")
    aliases = set(ALIAS.findall(text))
    for lineno, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith("'") or stripped.startswith("skinparam") or stripped.startswith("@start"):
            continue
        line = re.sub(r"#[0-9A-Fa-f]{3,8}\b", "", line)  # colour codes
        line = line.replace("\\n", " ")                    # PlantUML line breaks
        for rx in (CAMEL, MACRO):
            for m in rx.finditer(line):
                found.setdefault(m.group(0), lineno)
        for m in FUNC.finditer(line):
            found.setdefault(m.group(1), lineno)
    return {n: l for n, l in found.items() if n not in aliases}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*", type=Path)
    ap.add_argument("--strict", action="store_true", help="exit 1 if any name is missing")
    args = ap.parse_args()

    files = args.files or sorted((ROOT / "docs" / "uml").glob("arch_*.puml")) + sorted(
        (ROOT / "docs" / "uml").glob("seq_*.puml"))
    known = code_words() | allowed()
    in_actions = os.environ.get("GITHUB_ACTIONS") == "true"

    missing_total = 0
    for f in files:
        missing = {n: l for n, l in names_in(f).items() if n not in known}
        rel = f.resolve().relative_to(ROOT) if f.resolve().is_relative_to(ROOT) else f
        if not missing:
            print(f"ok       {rel}")
            continue
        missing_total += len(missing)
        print(f"MISSING  {rel}")
        for n, l in sorted(missing.items(), key=lambda kv: kv[1]):
            print(f"           line {l}: {n}")
            if in_actions:
                print(f"::warning file={rel},line={l}::UML names '{n}', which is not found in the code "
                      f"(renamed or removed?). Update the diagram or scripts/uml_names_allow.txt.")
    if missing_total:
        print(f"\n{missing_total} name(s) not found in include/, src/ or tests/.")
    return 1 if (missing_total and args.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
