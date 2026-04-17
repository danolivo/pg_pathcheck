# Analysis of `make check-world` run with pg_pathcheck enabled

Raw data: [report.txt](report.txt) — produced by `scripts/gather-warnings.sh`
against a complete `make check-world` pass on PG master with pg_pathcheck
loaded via `shared_preload_libraries` through `TEMP_CONFIG`. This document
summarises the patterns visible in 24,666 DETAIL records.

## Headline numbers

| Bucket | Count | % |
|---|---:|---:|
| Total DETAIL records | 24,666 | 100% |
| Unique signatures (after normalisation) | ~700 | — |
| INVALID at position `[1]` | 24,093 | 97.7% |
| INVALID at position `[2]` | 573 | 2.3% |
| Pathlist length 2 | 24,063 | 97.6% |
| Pathlist length 3 | 565 | 2.3% |
| Pathlist length 4 or 6 | 38 | 0.15% |

The overwhelming shape is a **two-element pathlist** with a valid Path at
`[0]` and garbage at `[1]`. That is the classic `add_path()` outcome: a
new path arrives, dominates the existing entry (or vice versa), and the
loser gets freed without being removed from the list — see `add_path()`
in `pathnode.c`, which `pfree`s dominated paths in place.

## First-element (the live survivor)

```
  19801  T_ProjectionPath   (80.3%)
   2789  T_HashPath         (11.3%)
   1149  T_NestPath         ( 4.7%)
    615  T_AggPath          ( 2.5%)
    108  T_SetOpPath        ( 0.4%)
     72  T_AppendPath       ( 0.3%)
     42  T_ForeignPath      ( 0.2%)
     36  T_SubqueryScanPath ( 0.1%)
     18  T_MergePath
     18  T_GatherPath
     12  T_UniquePath
      6  T_WindowAggPath
```

**`T_ProjectionPath` at 80%** is the loudest signal in the whole report.
It is consistent with the `apply_scanjoin_target_to_paths` hypothesis
from the survey email: that function walks rels, wraps/replaces each
existing path with a `ProjectionPath`, and can leave the old list entries
pointing at freed chunks. Because virtually every base rel goes through
scan-join target application, the ProjectionPath wrapper dominates the
finding space.

`T_HashPath`, `T_NestPath`, `T_AggPath` are the join and aggregation
variants of the same pattern.

## Second-element (the INVALID slot)

```
  13440  T_SeqScan          (54.5%)   ← Plan node
   8018  UNDEF(anything)    (32.5%)
   2654  T_BitmapHeapScan   (10.8%)   ← Plan node
    188  T_FunctionScan               ← Plan node
    144  T_Append                     ← Plan node
     51  T_NestLoop                   ← Plan node
     38  T_HashJoin                   ← Plan node
     36  T_IndexScan                  ← Plan node
     36  T_SubPlan                    ← Plan / expr node
     21  T_TableFuncScan              ← Plan node
     10  T_Agg                        ← Plan node
      6  T_IndexOnlyScan              ← Plan node
      6  T_ForeignScan                ← Plan node
      6  T_Alias                      ← parser node (!)
      6  T_Aggref                     ← expression node (!)
```

Every non-UNDEF tag is a **Plan** node, except `T_Alias` and `T_Aggref`,
which are surprises we'll come back to. The plan-node dominance is
exactly the 128-byte allocator-class aliasing the survey predicted:
SeqScan (112 B), BitmapHeapScan (120 B), NestLoop (128 B), and the rest
fit the chunk class previously held by Path (80 B) / ProjectionPath
(96 B) / NestPath (112 B). For the 256-byte class, HashPath (136 B) maps
to HashJoin (152 B), IndexScan (168 B), etc.

| Allocator class | Freed parent | Common aliased occupant |
|---|---|---|
| **128 B** | Path, ProjectionPath, NestPath, AggPath | SeqScan (54.5%), BitmapHeapScan (10.8%), FunctionScan, NestLoop, Append |
| **256 B** | HashPath, MergePath | HashJoin, IndexScan, Agg, IndexOnlyScan |

The proportions match the 128/256 class populations almost exactly —
128-class hits vastly outnumber 256-class hits, which matches the fact
that the pathlist-bearing parent types are themselves mostly 128-class.

## UNDEF breakdown

```
  UNDEF(0):               2793
  UNDEF(0x7F7F7F7F):         0   ← notable absence
  UNDEF(negative int):    5024
  UNDEF(positive int):     201
```

Two things worth calling out.

**No `0x7F7F7F7F` hits.** That is the `CLOBBER_FREED_MEMORY` signature —
`wipe_mem()` in `aset.c` memset's freed chunks to `0x7F`. Zero sightings
mean **every freed Path slot in this dataset was reallocated before
`planner_shutdown_hook` had a chance to observe it**. Under a busy
check-world the allocator freelist churns fast enough that the raw
clobber signature never survives. The detection tool isn't finding
"memory with 0x7F in it" — it's finding "memory that has been reused for
something else". This is an important framing point: if someone claims
`CLOBBER_FREED_MEMORY` would have found the same bugs on its own, point
at this zero and explain why it didn't.

**`UNDEF(negative int)` = 5,024** is also informative. Those values are
too random to be NodeTags and too specific to be the `0x7F` fill — they
look like pointer fragments or raw payload bytes being read as `int32`
NodeTag. This aligns with the chunk having been reused for a
**non-Node** allocation (a plain palloc'd array, a Bitmapset, a cost
array). In these cases the tool correctly flags the slot as garbage but
cannot name what is currently sitting in it.

**`UNDEF(0)` = 2,793** is the third sub-bucket. Two plausible sources:

- Freshly `palloc0`'d zeroed memory whose NodeTag has not been set yet
  by the allocating code when we observe it.
- Deliberate `type = T_Invalid` stamps in node post-processing.

## The `T_Alias` and `T_Aggref` outliers

Both appear only 6 times, but they are not Plan nodes:

- `T_Alias` — parser node, ~32 bytes.
- `T_Aggref` — expression node, ~96 bytes.

For these to show up, the freed Path slot must have been recycled into
**expression-tree allocation** — plausibly during `set_plan_references`
or expression simplification. `T_Aggref` fits the 128-byte class
directly; `T_Alias` is smaller than the class, so the hit is either
coincidental (a smaller allocation landing in a bucket that was
previously a Path) or a sign of cross-bucket activity.

These are worth keeping as a "the aliasing surface extends past Plan
nodes" data point.

## Source distribution

| Area | Buckets |
|---|---:|
| core regress | 342 |
| pg_upgrade | 336 |
| pg_dump | 92 |
| postgres_fdw | 35 |
| test_modules | 21 |
| other | 40 |

**pg_upgrade is nearly tied with core regress** — not because it exposes
a qualitatively different bug surface, but because it runs the core
regression suite *twice* (against the old cluster and the new), roughly
doubling up the same signatures with slightly different memory layouts.
The `UNDEF(random)` buckets defeat our deduplication across the two
runs, so the unique bug-class count is closer to ~400 once that is
accounted for.

**postgres_fdw's 35 buckets** are disproportionately `T_ForeignPath` in
the first position (21 hits of `[0] T_ForeignPath; [1] UNDEF(0)
INVALID`). That is the FDW upper-path code path the survey email does
not currently cover — worth calling out specifically as a distinct
sub-surface.

## Notable long-list findings

Three signatures have INVALID **in the middle** of the pathlist, with
valid paths on both sides — a qualitatively different shape:

```
  24  [0] T_AppendPath;       [1] T_AggPath;        [2] T_SeqScan INVALID; [3] T_AggPath

  12  [0] T_HashPath;         [1] T_HashPath;       [2] T_IndexScan INVALID;
      [3] T_NestPath;         [4] T_NestPath;       [5] T_MergePath

   2  [0] T_ProjectionPath;   [1] T_ProjectionPath; [2] UNDEF(…) INVALID;
      [3] T_ProjectionPath;   [4] T_ProjectionPath; [5] T_ProjectionPath
```

These rule out the simple "trailing dominated path was pfree'd"
explanation. Three plausible causes:

1. **Mid-list dominance** — `add_path()` removes elements via
   `list_delete_cell()` when a new path dominates an existing one, but
   under some control flow the cell remains live with its Path `pfree`d.
2. **In-place mutation** — some code re-used a Path struct for a
   different purpose without updating the list.
3. **Eager-aggregation double-agg** — the 24-hit signature
   (`T_AppendPath; T_AggPath; T_SeqScan INVALID; T_AggPath`) with two
   AggPaths straddling a dead slot suggests eager aggregation building a
   partial `grouped_rel`'s pathlist and leaving a stale entry from
   before the rewrite.

Small counts, but **qualitatively the most interesting** findings — they
invalidate the simplest mental model of the bug and are the ones where a
concrete reproducer would land best in hackers discussion.

## What is notably absent

- **No `0x7F7F7F7F` (raw CLOBBER signature).** Every freed slot is
  reused immediately.
- **No `T_Path` at position `[0]`.** The survivor is always a compound
  or wrapping path type (Projection, Join, Agg, Append, …).
- **No parent-mismatch records.** Phase 3 of the gather script produced
  an empty section, meaning either the parent-match code was not yet
  enabled at report time, or all the aliasing produces non-Path types
  (already caught by the tag check) rather than valid Paths with wrong
  owners. Worth re-running with the current HEAD to distinguish.

## Takeaways for the survey email

Three things that can be added with confidence:

1. **Quantify the ProjectionPath dominance:** 80% of all findings sit
   behind a ProjectionPath, pointing the finger at
   `apply_scanjoin_target_to_paths` as the single largest source.
2. **The reuse signature is ~65% Plan nodes from the 128-byte class**,
   concentrated in SeqScan and BitmapHeapScan — mechanically explained
   by allocator size-class matching.
3. **pg_upgrade doubles the headline count** without adding new bug
   classes; the unique-bug count is closer to ~400.

And one specific data point worth mentioning for partitioning / FDW
reviewers: **postgres_fdw has its own ForeignPath-dominance signature**
that the survey's current list does not mention.

## Reproducing this analysis

```bash
# Against the same report file.
cd contrib/pg_pathcheck

# Distribution by first-element path type.
awk '/^ *[0-9]+  pathlist contents:/ {
       if (match($0, /\[0\] T_[A-Za-z_]+/) > 0) {
         tag = substr($0, RSTART+4, RLENGTH-4)
         by_first[tag] += $1
       }
     }
     END { for (t in by_first) printf "%10d  %s\n", by_first[t], t }' \
     docs/report.txt | sort -rn

# INVALID position distribution.
awk '/^ *[0-9]+  pathlist contents:/ {
       if (match($0, /\[[0-9]+\][^;]*INVALID/) > 0) {
         s = substr($0, RSTART, RLENGTH)
         match(s, /\[[0-9]+\]/)
         pos = substr(s, RSTART+1, RLENGTH-2)
         by_pos[pos] += $1
       }
     }
     END { for (p in by_pos) printf "  INVALID at [%s]: %d\n", p, by_pos[p] }' \
     docs/report.txt | sort
```
