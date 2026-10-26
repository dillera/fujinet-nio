#!/usr/bin/env bash
# Renders the UML diagrams to build/uml/*.svg: the hand-authored ones in
# docs/uml (arch_*, seq_*) and the code-derived ones (gen_*, from clang-uml).
# None of it is committed; the "Publish UML diagrams" workflow publishes the
# result to GitHub Pages (scripts/build_site.py builds the page).
#
#   scripts/gen_uml.sh               build + run the unit tests, generate gen_*,
#                                    render everything (needs clang-uml, plantuml)
#   scripts/gen_uml.sh --render-only render only the hand-authored diagrams, for
#                                    previewing an edit (needs plantuml)
#
# Preview the page locally:
#   scripts/build_site.py --svg-dir build/uml --out build/site
#   python3 -m http.server -d build/site
set -euo pipefail
set -x

OUT=build/uml

if [ "${1:-}" = "--render-only" ]; then
    mkdir -p "$OUT"
    plantuml -v --duration -tsvg docs/uml/arch_*.puml docs/uml/seq_*.puml -o "$PWD/$OUT"
    exit 0
fi

echo "Generating UML diagrams..."
date

BUILD_TARGET=fujibus-pty-debug

./build.sh -cp ${BUILD_TARGET}

mkdir -p docs/uml "$OUT"

# remove old files in case their UML is removed.
rm -f docs/uml/gen_*.puml "$OUT"/*.svg

# generate new versions
clang-uml -c clang-uml.yml -d build/${BUILD_TARGET}
plantuml -v --duration -tsvg docs/uml/*.puml -o "$PWD/$OUT"

python3 -I scripts/check_uml_names.py || true   # warns about stale names in hand-authored diagrams

echo "Done generating UML diagrams."
date
