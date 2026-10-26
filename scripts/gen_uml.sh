#!/usr/bin/env bash
# Generates the code-derived UML diagrams (gen_*), after building and testing.
#
#   scripts/gen_uml.sh                 build + test, then generate gen_*.svg
#                                      into docs/images (not committed; the
#                                      "Publish UML diagrams" workflow
#                                      publishes them to GitHub Pages)
#   scripts/gen_uml.sh --render-docs   render the hand-authored diagrams
#                                      (arch_*, seq_*) in docs/uml to
#                                      docs/images; commit those yourself
set -euo pipefail
set -x

if [ "${1:-}" = "--render-docs" ]; then
    mkdir -p docs/images
    plantuml -v --duration -tsvg docs/uml/arch_*.puml docs/uml/seq_*.puml -o ../images
    exit 0
fi

echo "Generating UML diagrams..."
date

BUILD_TARGET=fujibus-pty-debug

./build.sh -cp ${BUILD_TARGET}

mkdir -p docs/images
mkdir -p docs/uml

# remove old files in case their UML is removed.
rm -f docs/uml/gen_*.puml docs/images/gen_*.svg

# generate new versions
clang-uml -c clang-uml.yml -d build/${BUILD_TARGET}
plantuml -v --duration -tsvg docs/uml/gen_*.puml -o ../images

echo "Done generating UML diagrams."
date
