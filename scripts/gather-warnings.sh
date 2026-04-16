#!/usr/bin/env bash
#
# gather-warnings.sh
#
# Crawl a PostgreSQL build tree after `make check` / `make check-world`
# (or `make installcheck`) and collect pg_pathcheck DETAIL lines,
# grouping identical ones and printing occurrence counts.
#
# Usage:
#   gather-warnings.sh [pg-source-root] [pattern]
#
#     pg-source-root   directory to search (default: current directory)
#     pattern          substring that identifies our DETAILs
#                      (default: "contents:" — matches "pathlist contents:",
#                       "partial_pathlist contents:", etc.)
#
# Locations swept (covers both pg_regress and TAP output):
#
#   */log/postmaster.log             regress-style server log
#   */log/*.log                      per-test server logs (tmp_check)
#   */log/regress_log_*              TAP orchestrator logs
#   */tmp_check/log/*                TAP server + orchestrator logs
#   */output_iso/log/*               isolation-test server logs
#   */output_iso/results/*.out       isolation-test psql output
#   */results/*.out                  regression psql output
#   regression.diffs (anywhere)      failed-test diffs
#

set -euo pipefail

ROOT="${1:-.}"
PATTERN="${2:-contents:}"

if [ ! -d "$ROOT" ]; then
	echo "usage: $(basename "$0") [pg-source-root] [pattern]" >&2
	exit 2
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

#
# Phase 1 — locate candidate files and grep their DETAIL lines.
# The DETAIL marker is emitted both by server logs ("\tDETAIL:  ...") and
# by psql transcript output ("DETAIL:  ..."), so we match the substring
# anywhere on the line and strip everything up to and including "DETAIL:".
#
find "$ROOT" \( \
		-path '*/log/postmaster.log' \
	-o	-path '*/log/*.log' \
	-o	-path '*/log/regress_log_*' \
	-o	-path '*/tmp_check/log/*' \
	-o	-path '*/output_iso/log/*' \
	-o	-path '*/output_iso/results/*.out' \
	-o	-path '*/results/*.out' \
	-o	-name 'regression.diffs' \
	\) -type f -print0 2>/dev/null \
| while IFS= read -r -d '' f; do
	awk -v src="$f" -v pat="$PATTERN" '
		/DETAIL:/ && index($0, pat) > 0 {
			line = $0
			sub(/^.*DETAIL:[ \t]*/, "", line)
			printf("%s\t%s\n", src, line)
		}
	' "$f"
done > "$tmp"

#
# Phase 2 — summarise.
#
if [ ! -s "$tmp" ]; then
	echo "No '$PATTERN' DETAIL lines found under $ROOT"
	exit 0
fi

total=$(wc -l < "$tmp" | tr -d ' ')

echo "=== pg_pathcheck findings under $ROOT ==="
echo "Total DETAIL records: $total"
echo

# Group on the normalised DETAIL text.  Scrub pointer addresses and numeric
# rel-array indexes so cosmetic variance does not create separate buckets.
# Keep the first-seen source file as the representative.
awk -F'\t' '
	{
		src = $1; det = $2
		gsub(/0x[0-9a-fA-F]+/, "0xADDR", det)
		gsub(/rel#[0-9]+/, "rel#N", det)
		if (!(det in cnt))
			first_src[det] = src
		cnt[det]++
	}
	END {
		for (d in cnt)
			printf("%d\t%s\t%s\n", cnt[d], d, first_src[d])
	}
' "$tmp" | sort -t$'\t' -k1,1 -rn \
| while IFS=$'\t' read -r n detail src; do
	printf '%5d  %s\n' "$n" "$detail"
	printf '       first seen in: %s\n' "$src"
	echo
done
