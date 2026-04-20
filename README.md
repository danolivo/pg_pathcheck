<p align="center">
  <img src="pics/banner-readme-dark.jpg" alt="pg_pathcheck banner" />
</p>

# pg_pathcheck

PostgreSQL extension that validates the planner's final Path tree, detecting freed or corrupt memory by walking every reachable Path and checking NodeTags.
Reports and analyses of findings are published on the [project wiki](https://github.com/danolivo/pg_pathcheck/wiki).

## How it works

The extension uses `create_upper_paths_hook` and `planner_shutdown_hook`. It walks the entire Path tree rooted at the top `PlannerInfo`, then recurses into every subquery subroot reachable via `RelOptInfo::subroot`, every subplan subroot in `PlannerGlobal::subroots`, and every parallel RelOptInfo (`unique_rel`, `grouped_rel`, `part_rels`). For each visited RelOptInfo, it inspects `pathlist`, `partial_pathlist`, `cheapest_parameterized_paths`, and the `cheapest_startup_path` / `cheapest_total_path` singletons. Compound Path nodes embed further Path pointers (`outerjoinpath`/`innerjoinpath`, `subpath`, `subpaths`, `bitmapqual`, ...); the walker descends into each, so a corrupt pointer one level down is still caught.

Two detection mechanisms run together. The first is a NodeTag whitelist: if `path->type` is not one of the known Path-family node tags, the memory has been freed (with `CLOBBER_FREED_MEMORY` the bytes read as `0x7F`) or reallocated for a node of a different class. The second is a parent-match check on base and join rels: every Path found directly on the rel's own lists must carry `path->parent == rel`. A mismatch catches same-size-class aliasing, where the freed slot has been recycled into another Path that passes the tag test — the parent pointer reveals the slot has been claimed by a different owner.

## Installing

The `pg_pathcheck` targets **PostgreSQL master**. Since it is a pure module without any UI registered in the database, it uses `PG_MODULE_MAGIC_EXT` to expose the code version and the `extension_state` slot API. For better results, use a **cassert build** so that `CLOBBER_FREED_MEMORY` is enabled. A typical configure line:

```bash
./configure --prefix=/path/to/install \
    --enable-cassert --enable-debug --enable-injection-points
```

The cleanest way to build and install the extension is in-tree, so that `make install` carries it into `$libdir` alongside core.

Standalone PGXS builds also work:

```bash
cd /path/to/pg_pathcheck
USE_PGXS=1 PG_CONFIG=/path/to/install/bin/pg_config make install
```

There is no `CREATE EXTENSION pg_pathcheck`. It registers no SQL objects; its entire effect is through planner hooks activated at library load.

## Running

### Option 1: Preload into a single running server

```bash
initdb -D pgdata
echo "shared_preload_libraries = 'pg_pathcheck'" >> pgdata/postgresql.conf
pg_ctl -D pgdata -l pgdata/logfile start

# ... then run whatever you want to check
psql -c 'EXPLAIN SELECT ...'
make installcheck
```

### Option 2: preload into every cluster `make check-world` spawns

The PG test harness honours the `TEMP_CONFIG` environment variable: whichever file it points at is appended to every cluster's `postgresql.conf` — both clusters created by `pg_regress` (via `--temp-config`) and clusters spawned by `PostgreSQL::Test::Cluster->init()` (via `$ENV{TEMP_CONFIG}`).

```bash
cat > /tmp/ppc.conf <<'EOF'
shared_preload_libraries = 'pg_pathcheck'
EOF

TEMP_CONFIG=/tmp/ppc.conf make check-world
```

Every test cluster — core regression, TAP tests, isolation, ECPG, contrib modules, and PL tests — will load pg_pathcheck at startup and emit warnings into its own `tmp_check/log/*.log`.

### Option 3: focus on a single test file

```bash
cd ~/pg/src/test/recovery
TEMP_CONFIG=/tmp/ppc.conf \
    make check PROVE_TESTS='t/001_stream_rep.pl' PROVE_FLAGS='-v'
# Per-cluster logs land in ./tmp_check/log/
```

## GUCs

### `pg_pathcheck.elevel` — report elevel

```sql
SET pg_pathcheck.elevel = 'warning';  -- default, log and keep going
SET pg_pathcheck.elevel = 'error';    -- abort the offending statement
SET pg_pathcheck.elevel = 'panic';    -- PANIC → core dump
```

Use `error` when you want to pin down exactly which query triggers a finding in a regression run (the test will fail with the guilty query in the error message). Use `panic` when you want a core dump for post-mortem analysis of the bad pathlist.

### `pg_pathcheck.stage_checks` — per-stage tripwires

```sql
SET pg_pathcheck.stage_checks = off;  -- default
SET pg_pathcheck.stage_checks = on;   -- walk pathlists at every hook boundary
```

By default only the end-of-planning walker runs (at `planner_shutdown_hook`). That catches corruption but tells you nothing about *when* during planning it happened.

Turning `stage_checks` on adds three extra tripwires that fire during planning:

| Hook | Fires at | Pins a finding to |
|---|---|---|
| `set_rel_pathlist_hook` | end of `set_*_pathlist()` for each base rel | base-rel path generation |
| `set_join_pathlist_hook` | end of `populate_joinrel_with_paths()` | join-rel construction |
| `create_upper_paths_hook` | end of each `UPPERREL_*` stage | a specific upper-rel stage (ordering, grouping, distinct, …) |

Each tripwire checks every entry in the affected rel's `pathlist` / `partial_pathlist` for a valid Path NodeTag and — for base/join rels — a matching `->parent`. A finding carries a short context string (`"base rel"`, `"outer side of join rel {a,b}"`, `"create_upper_paths input, stage UPPERREL_ORDERED"`) that identifies which hook caught it.

Combine with `elevel = 'error'` to halt on the first earliest-firing finding:

```sql
SET pg_pathcheck.stage_checks = on;
SET pg_pathcheck.elevel       = 'error';
-- re-run the suspect query; the error message names the guilty stage.
```

Leave `stage_checks` off for day-to-day coverage — every extra walk multiplies planner overhead by the number of hook firings, which can be substantial for join-heavy queries.

## Understanding the output

A detection looks like this:

```
WARNING:  pg_pathcheck: invalid NodeTag T_SeqScan in pathlist, rel {orders, lineitem}
DETAIL:   pathlist contents: [0] T_ProjectionPath; [1] T_SeqScan INVALID
HINT:     query: SELECT ... FROM orders o JOIN lineitem l ...
```

Reading it:

- **`invalid NodeTag <tag>`** — the NodeTag read from a Path pointer. If it is `UNDEF(n)`, the 32-bit value at offset 0 of the chunk is not in PostgreSQL's known-tag range. If it is a real tag but not a Path (like `T_SeqScan` above), the chunk has been freed and reallocated for a different node type.
- **`in <source>`** — the rel-level slot where the stray pointer was found: `pathlist`, `partial_pathlist`, `cheapest_startup_path`, `cheapest_total_path`, `cheapest_parameterized_paths`, or a compound-path field like `SortPath.subpath`.
- **`rel {a, b}`** — the owning RelOptInfo resolved via `relids` → `simple_rte_array[i]->eref->aliasname`. `{}` (empty braces) means an upper rel with no base-rel members; `(upper)` labels query-wide upper rels; `(unknown)` means the name could not be resolved.
- **`DETAIL`** — full contents of the containing list, with each element tagged by its node kind; the guilty one is marked `INVALID`.
- **`HINT`** — `debug_query_string` at the time of detection, so you can correlate a finding with a specific user query.

The second variant — *parent mismatch* — indicates same-size-class aliasing:

```
WARNING:  pg_pathcheck: path parent mismatch in pathlist, target rel {orders}
DETAIL:   path T_SortPath claims rel {} (upper), ...
HINT:     query: ...
```

Here the NodeTag is a legitimate Path, but `path->parent` does not match the rel whose list was being walked — the slot was freed, and a new Path belonging to another rel now sits there.

## Collecting findings across a large test run

After a `make check-world`, warnings are scattered across dozens of `tmp_check/log/*.log` files, per-subdir `log/postmaster.log`, `*.out` results, and `regression.diffs`. Use the helper:

```bash
./scripts/gather-warnings.sh /path/to/pg_src > report.txt
```

The script produces two sections:

1. **Deduplicated DETAIL summary** — groups identical pathlist signatures (addresses normalised, numeric fallback labels collapsed), shows counts and the first log file that recorded each one.
2. **Parent-mismatch findings** — groups by `(field, target rel, claimed rel)` tuples so aliasing patterns jump out.

A typical line in section 1:

```
   800  pathlist contents: [0] T_HashPath; [1] T_SeqScan INVALID
        first seen in: pg_src/src/test/regress/regression.diffs
```

means 800 occurrences of the same signature were found; opening the indicated `regression.diffs` file will show which test triggered the first one.

## When something fires in your code

If you are running pg_pathcheck against a patch you are writing or an extension you maintain, and it flags something that was not there on the unmodified master:

1. **Find the triggering query.** The `HINT:` line names it; re-run with `SET pg_pathcheck.elevel = 'error'` to get the exact test query that causes the fault.
2. **Identify the rel and field.** `target rel {a, b} in cheapest_startup_path` names both the RelOptInfo and the specific slot that is stale.
3. **Use the DETAIL list to reconstruct what should have been there.** The valid siblings in the list are a strong hint about the path type that was originally stored in the bad slot.
4. **Check your `pfree` / `list_delete_cell` pairing.** Most findings reduce to "I freed a path but did not remove it from the list" or "I kept a pointer to a path across a stage that frees it".
5. **If the fix is in core planner code**, please share it on pgsql-hackers — this is the wider discussion pg_pathcheck is meant to feed.

## Continuous coverage

The repo ships a GitHub Actions workflow (`.github/workflows/regress.yml`) that runs `make -k check-world` with `pg_pathcheck` loaded on every push to `main`, on every PR, on manual dispatch, and nightly. Artefacts include the raw server logs and a summary written to the job's step-summary panel. Point your fork or extension repo at the same workflow if you would like continuous coverage against PG master as it evolves.

## Disclaimer

Most of the code in this repository — including the extension itself, the CI workflow, the helper scripts, and portions of the documentation — was generated in collaboration with a large language model (Claude). Every change was reviewed and directed by a human before being committed, but the prose and structure are largely machine-produced. Please read the code rather than trusting the comments, and report any issues upstream.
