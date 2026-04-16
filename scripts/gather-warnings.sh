#!/usr/bin/env bash
#
# gather-warnings.sh
#
# Crawl a PostgreSQL build tree after `make check` / `make check-world`
# (or `make installcheck`) and collect every pg_pathcheck WARNING together
# with its DETAIL line.  Deduplicates by normalised DETAIL text (pointer
# addresses replaced with 0xADDR) and prints occurrence counts.
#
# Usage:
#   gather-warnings.sh [pg-source-root] [pattern]
#
#     pg-source-root   directory to search (default: current directory)
#     pattern          substring inside the WARNING to match
#                      (default: pg_pathcheck)
#
# Test output produced by check / check-world lands in a handful of
# well-known locations; we sweep all of them:
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
# WARNINGs coming out of the server log carry a timestamp prefix, while
# those echoed to psql do not; the extractor handles both.
#

set -euo pipefail

ROOT="${1:-.}"
PATTERN="${2:-pg_pathcheck}"

if [ ! -d "$ROOT" ]; then
	echo "usage: $(basename "$0") [pg-source-root] [pattern]" >&2
	exit 2
fi

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

#
# Phase 1 — locate candidate files.
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
	#
	# Phase 2 — extract WARNING+DETAIL pairs.
	#
	# Cheap filter before running awk.
	grep -qF "$PATTERN" "$f" 2>/dev/null || continue

	awk -v src="$f" -v pat="$PATTERN" '
		function trim_warning(s,  r) {
			r = s
			sub(/^.*WARNING:[ \t]*/, "", r)
			return r
		}
		function trim_detail(s,  r) {
			r = s
			sub(/^.*DETAIL:[ \t]*/, "", r)
			return r
		}

		# Candidate WARNING line.  index() is a plain-text check that
		# avoids surprises from regex-metacharacters in $PATTERN.
		/WARNING:/ && index($0, pat) > 0 {
			if (warning != "")
				printf("%s\t%s\t(no DETAIL)\n", src, warning)
			warning = trim_warning($0)
			next
		}
		warning != "" && /DETAIL:/ {
			printf("%s\t%s\t%s\n", src, warning, trim_detail($0))
			warning = ""
			next
		}
		# Any non-continuation log line closes the pending WARNING
		# without a DETAIL (shouldn'\''t happen for pg_pathcheck, but be
		# safe if other log messages interleave).
		warning != "" && /WARNING:|ERROR:|FATAL:|PANIC:|LOG:|STATEMENT:/ {
			printf("%s\t%s\t(no DETAIL)\n", src, warning)
			warning = ""
		}
		END {
			if (warning != "")
				printf("%s\t%s\t(no DETAIL)\n", src, warning)
		}
	' "$f" >> "$tmp"
done

#
# Phase 3 — summarise.
#
if [ ! -s "$tmp" ]; then
	echo "No '$PATTERN' warnings found under $ROOT"
	exit 0
fi

total=$(wc -l < "$tmp" | tr -d ' ')

echo "=== pg_pathcheck findings under $ROOT ==="
echo "Total records: $total"
echo

# Deduplicate on the normalised DETAIL (addresses scrubbed).  Keep the
# first-seen source file and WARNING as representatives.
awk -F'\t' '
	{
		src = $1; warn = $2; det = $3
		# Normalise pointer addresses so cosmetic differences between
		# runs do not create separate buckets.
		gsub(/0x[0-9a-fA-F]+/, "0xADDR", det)
		gsub(/rel#[0-9]+/, "rel#N", det)
		if (!(det in cnt)) {
			first_src[det] = src
			first_warn[det] = warn
		}
		cnt[det]++
	}
	END {
		for (d in cnt)
			printf("%d\t%s\t%s\t%s\n",
				   cnt[d], d, first_warn[d], first_src[d])
	}
' "$tmp" | sort -t$'\t' -k1,1 -rn \
| while IFS=$'\t' read -r n detail warn src; do
	printf '%5d  %s\n' "$n" "$detail"
	printf '       WARNING: %s\n' "$warn"
	printf '       first seen in: %s\n' "$src"
	echo
done
