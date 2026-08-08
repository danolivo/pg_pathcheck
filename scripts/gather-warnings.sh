#!/usr/bin/env bash
#
# gather-warnings.sh
#
# Crawl a PostgreSQL build tree after `make check` / `make check-world`
# (or `make installcheck`) and collect pg_pathcheck findings, grouping
# identical ones and printing occurrence counts.
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
#   *.diffs (anywhere)               failed-test diffs
#
# The message shapes this parses are produced by ppc_check_tag() and
# ppc_check_parent() in pg_pathcheck.c:
#
#   WARNING: pg_pathcheck: invalid NodeTag T_SeqScan in pathlist[1], rel {t}
#   DETAIL:  detected at end of planning; pathlist contents: [0] ...
#
#   WARNING: pg_pathcheck: path parent mismatch in pathlist[1], rel {t}
#   DETAIL:  detected at base rel; path T_SortPath claims rel {u}; rows ...
#
# Keep them in step: this script matches on " in ", ", rel " and
# " claims rel " and will silently report nothing if those move.
#

set -euo pipefail

ROOT="${1:-.}"
PATTERN="${2:-contents:}"

if [ ! -d "$ROOT" ]; then
	echo "usage: $(basename "$0") [pg-source-root] [pattern]" >&2
	exit 2
fi

filelist=$(mktemp)
tmp=$(mktemp)
tmp2=$(mktemp)
trap 'rm -f "$filelist" "$tmp" "$tmp2"' EXIT

#
# Enumerate candidate files once.  A check-world tree is large enough that
# walking it twice is a noticeable chunk of this script's runtime.
#
find "$ROOT" \( \
		-path '*/log/postmaster.log' \
	-o	-path '*/log/*.log' \
	-o	-path '*/log/regress_log_*' \
	-o	-path '*/tmp_check/log/*' \
	-o	-path '*/output_iso/log/*' \
	-o	-path '*/output_iso/results/*.out' \
	-o	-path '*/results/*.out' \
	-o	-name '*.diffs' \
	\) -type f -print0 2>/dev/null > "$filelist" || true

#
# Phase 1 — grep DETAIL lines.
# The DETAIL marker is emitted both by server logs ("\tDETAIL:  ...") and
# by psql transcript output ("DETAIL:  ..."), so we match the substring
# anywhere on the line and strip everything up to and including "DETAIL:".
#
# We then cut the leading "detected at <context>; " that pg_pathcheck
# prepends, keeping the record from "<listname> contents:" onwards.  That
# keeps buckets comparable with reports gathered before the context field
# existed, and stops the same corruption from splitting across buckets
# just because two hooks caught it.
#
while IFS= read -r -d '' f; do
	awk -v src="$f" -v pat="$PATTERN" '
		/DETAIL:/ && index($0, pat) > 0 {
			line = $0
			sub(/^.*DETAIL:[ \t]*/, "", line)

			# Trim back to the "<listname> contents:" phrase if present.
			p = index(line, "contents:")
			if (p > 1) {
				pre = substr(line, 1, p - 1)
				sub(/[ \t]+$/, "", pre)
				n = split(pre, parts, /[ \t;]+/)
				if (n > 0 && parts[n] != "")
					line = parts[n] " " substr(line, p)
			}
			printf("%s\t%s\n", src, line)
		}
	' "$f"
done < "$filelist" > "$tmp"

#
# Phase 2 — summarise.
#
if [ ! -s "$tmp" ]; then
	echo "No '$PATTERN' DETAIL lines found under $ROOT"
else
	total=$(wc -l < "$tmp" | tr -d ' ')

	echo "=== pg_pathcheck findings under $ROOT ==="
	echo "Total DETAIL records: $total"
	echo

	# Group on the normalised DETAIL text.  Scrub pointer addresses and
	# numeric rel-array indexes so cosmetic variance does not create
	# separate buckets.  Keep the first-seen source file as representative.
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
fi

#
# Phase 3 — parent-mismatch analysis.
#
# Pair WARNING lines reporting "parent mismatch" or "non-RelOptInfo parent"
# with their DETAIL (if any) to extract three fields:
#
#   - field:   the rel-level slot where the stray pointer was found
#              (pathlist[1], cheapest_startup_path, ...)
#   - target:  the rel whose slot was being walked
#   - claims:  the rel the path's ->parent actually points at
#
# Group by (field, target, claims) so aliasing patterns jump out.
#
while IFS= read -r -d '' f; do
	awk -v src="$f" '
		BEGIN { pending = 0 }

		function starts_record(l) {
			return (l ~ /WARNING:/ || l ~ /ERROR:/ || l ~ /FATAL:/ ||
			        l ~ /PANIC:/  || l ~ /LOG:/   || l ~ /STATEMENT:/)
		}

		# Any new log record finalises the previous one (for the
		# "non-RelOptInfo parent" variant, whose DETAIL carries no
		# "claims rel" phrase).
		starts_record($0) && pending {
			printf("%s\t%s\t(no DETAIL)\t%s\n", fld, tgt, src)
			pending = 0
		}

		index($0, "pg_pathcheck") > 0 &&
		(index($0, "parent mismatch") > 0 ||
		 index($0, "non-RelOptInfo parent") > 0) {
			line = $0
			sub(/^.*pg_pathcheck:[ \t]*/, "", line)
			# line now starts with one of:
			#   path parent mismatch in FIELD, rel TGT
			#   path has non-RelOptInfo parent in FIELD, rel TGT
			i1 = index(line, " in ")
			if (i1 == 0) { pending = 0; next }
			after = substr(line, i1 + 4)
			i2 = index(after, ", rel ")
			if (i2 == 0) { pending = 0; next }
			fld = substr(after, 1, i2 - 1)
			tgt = substr(after, i2 + length(", rel "))
			sub(/[ \t]+$/, "", tgt)
			pending = 1
			next
		}

		# "detected at CTX; path TAG claims rel CLAIM; rows ..."
		pending && /DETAIL:/ && index($0, " claims rel ") > 0 {
			line = $0
			i = index(line, " claims rel ")
			claim = substr(line, i + length(" claims rel "))
			semi = index(claim, ";")
			if (semi > 0)
				claim = substr(claim, 1, semi - 1)
			sub(/[ \t]+$/, "", claim)
			printf("%s\t%s\t%s\t%s\n", fld, tgt, claim, src)
			pending = 0
			next
		}

		END {
			if (pending)
				printf("%s\t%s\t(no DETAIL)\t%s\n", fld, tgt, src)
		}
	' "$f"
done < "$filelist" > "$tmp2"

if [ -s "$tmp2" ]; then
	total2=$(wc -l < "$tmp2" | tr -d ' ')
	echo
	echo "=== Parent-mismatch findings ==="
	echo "Total parent-mismatch records: $total2"
	echo

	awk -F'\t' '
		{
			fld = $1; tgt = $2; claim = $3; src = $4
			# Collapse the list index so pathlist[0] and pathlist[7] share
			# a bucket, and normalise fallback "rel#N" labels.  Alias names
			# are kept intact.
			gsub(/\[[0-9]+\]/, "[N]", fld)
			gsub(/rel#[0-9]+/, "rel#N", tgt)
			gsub(/rel#[0-9]+/, "rel#N", claim)
			key = fld "|" tgt "|" claim
			if (!(key in cnt))
				first_src[key] = src
			cnt[key]++
		}
		END {
			for (k in cnt) {
				n = split(k, a, "|")
				printf("%d\t%s\t%s\t%s\t%s\n",
				       cnt[k], a[1], a[2], a[3], first_src[k])
			}
		}
	' "$tmp2" | sort -t$'\t' -k1,1 -rn \
	| while IFS=$'\t' read -r n fld tgt claim src; do
		printf '%5d  %-32s target %-30s claims %s\n' \
		       "$n" "$fld" "$tgt" "$claim"
		printf '       first seen in: %s\n' "$src"
		echo
	done
fi

#
# Phase 4 — walker-staleness tripwire.
#
# "unhandled Path subtype" means core grew a Path type that walk_path()
# has no case for, so that node's sub-paths went unchecked.  It is a
# defect in pg_pathcheck itself rather than a finding about PostgreSQL,
# and it silently shrinks coverage, so surface it separately.
#
stale=$(mktemp)
trap 'rm -f "$filelist" "$tmp" "$tmp2" "$stale"' EXIT

while IFS= read -r -d '' f; do
	grep -ho "unhandled Path subtype [A-Za-z_0-9]*" "$f" 2>/dev/null || true
done < "$filelist" > "$stale"

if [ -s "$stale" ]; then
	echo
	echo "=== pg_pathcheck is out of date ==="
	sort "$stale" | uniq -c | sort -rn
	echo
	echo "Add a case to walk_path() for each subtype listed above."
fi
