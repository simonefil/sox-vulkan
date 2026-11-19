#!/usr/bin/env bash
# Generates the PDF (groff) and HTML (mandoc) manuals from the roff sources.
set -euo pipefail

cd "$(dirname "$0")"

for tool in groff ps2pdf mandoc; do
  command -v "$tool" >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

for page in sox.1 soxformat.7; do
  name=${page%.*}
  groff -t -man -Tps -dpaper=a4 -dMF=R -P-pa4 "$page" | ps2pdf -sPAPERSIZE=a4 - "$name.pdf"
  mandoc -Thtml -Ostyle=man.css,man='%N.html' "$page" > "$name.html"
done
