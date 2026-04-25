<p align="center">
  <img src="pics/banner-readme-dark.jpg" alt="pg_pathcheck banner" />
</p>

# pg_pathcheck

PostgreSQL extension that validates the planner's final Path tree, detecting freed or corrupt memory by walking every reachable Path and checking NodeTags.
Reports and analyses of findings are published on the [project wiki](https://github.com/danolivo/pg_pathcheck/wiki).

## Which branch do I want?

`pg_pathcheck` lives on more than one long-running Git branch. Each branch targets a different PostgreSQL release line; the **user-visible interface (name, GUCs, warning format) is identical across branches** so findings are directly comparable.

| If you run                          | Check out branch                                                                  | Tracks upstream                |
|-------------------------------------|-----------------------------------------------------------------------------------|--------------------------------|
| PostgreSQL **17.x** or **18.x**     | [`pg17-18`](https://github.com/danolivo/pg_pathcheck/tree/pg17-18)                | `REL_17_STABLE`, `REL_18_STABLE` |
| PostgreSQL **master / 19devel**     | [`master`](https://github.com/danolivo/pg_pathcheck/tree/master)                  | `master`                       |

You are reading the `pg17-18` branch's README. The implementation diverges from `master` because the available planner hooks differ between PG versions (notably `planner_shutdown_hook` and the `extension_state` slot API are master-only). On 17/18 the walker is driven inline from `create_upper_paths_hook` at `UPPERREL_FINAL`; on master it runs in the dedicated shutdown hook after `standard_planner` returns.

**Coverage caveat between branches.** Because the 17/18 walk fires before `create_plan` and `setrefs.c`, any Path-lifetime bug visible only during those later stages is caught on `master` but not on `pg17-18`. Base-, join- and upper-rel Path generation is complete by `UPPERREL_FINAL` so the bulk of the detection surface is the same on both branches.

## How it works

The extension hooks `create_upper_paths_hook`, `set_rel_pathlist_hook`, and `set_join_pathlist_hook`. The end-of-planning walker fires inline at `UPPERREL_FINAL` of the top query and traverses the Path tree rooted at the top `PlannerInfo`, recursing into every subquery subroot reachable via `RelOptInfo::subroot`, every subplan subroot in `PlannerGlobal::subroots`, and every partition child in `RelOptInfo::part_rels[]`. For each visited RelOptInfo it inspects `pathlist`, `partial_pathlist`, `cheapest_parameterized_paths`, and the `cheapest_{startup,total,unique}_path` singletons. Compound Path nodes embed further Path pointers (`outerjoinpath`/`innerjoinpath`, `subpath`, `subpaths`, `bitmapqual`, ...); the walker descends into each, including the per-aggregate access paths reachable through `MinMaxAggPath.mmaggregates`, so a corrupt pointer one level down is still caught.

Two detection mechanisms run together. The first is a NodeTag whitelist: if `path->type` is not one of the known Path-family node tags, the memory has been freed (with `CLOBBER_FREED_MEMORY` the bytes read as `0x7F`) or reallocated for a node of a different class. The second is a parent-match check on base and join rels: every Path found directly on the rel's own lists must carry `path->parent == rel`. A mismatch catches same-size-class aliasing, where the freed slot has been recycled into another Path that passes the tag test — the parent pointer reveals the slot has been claimed by a different owner.

## Installing

This branch of `pg_pathcheck` targets **PostgreSQL 17 and 18**. The extension is a pure module — it registers no SQL objects, only planner hooks activated at library load. PG 18 advertises the version string via `PG_MODULE_MAGIC_EXT`; PG 17 predates that macro and falls back to plain `PG_MODULE_MAGIC` (no version metadata is exposed). For better results, use a **cassert build** so that `CLOBBER_FREED_MEMORY` is enabled. A typical configure line:

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

The repository also ships a [PGXN](https://pgxn.org/)-ready `META.json`, so once a release is published on the network you will be able to install through [`pgxnclient`](https://pgxn.github.io/pgxnclient/) without cloning:

```bash
pgxn install pg_pathcheck      # picks the build matching your PostgreSQL version
```

The release tarball is `pg_pathcheck-<version>-pg17-18.zip`, produced by `make dist`; see [Versioning and releases](#versioning-and-releases) for how it is built and the choice you make at PGXN-upload time about whether the back-branch release is a separate distribution or a build-metadata-suffixed variant of the same one.

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

### `pg_pathcheck.end_walk` — master switch for the end-of-planning walk

```sql
SET pg_pathcheck.end_walk = on;       -- default
SET pg_pathcheck.end_walk = off;      -- silence the walker without unloading
```

Controls whether the walker fires at `UPPERREL_FINAL` of the top query. Turn off when you want to load the library purely for the per-stage tripwires (when `stage_checks` is on), or to silence the walker for a specific session without unloading the shared library.

### `pg_pathcheck.stage_checks` — per-stage tripwires

```sql
SET pg_pathcheck.stage_checks = off;  -- default
SET pg_pathcheck.stage_checks = on;   -- walk pathlists at every hook boundary
```

By default only the end-of-planning walker runs. That catches corruption but tells you nothing about *when* during planning it happened.

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

If you are running pg_pathcheck against a patch you are writing or an extension you maintain, and it flags something that was not there on the unmodified PG 17 / 18 stable branch:

1. **Find the triggering query.** The `HINT:` line names it; re-run with `SET pg_pathcheck.elevel = 'error'` to get the exact test query that causes the fault.
2. **Identify the rel and field.** `target rel {a, b} in cheapest_startup_path` names both the RelOptInfo and the specific slot that is stale.
3. **Use the DETAIL list to reconstruct what should have been there.** The valid siblings in the list are a strong hint about the path type that was originally stored in the bad slot.
4. **Check your `pfree` / `list_delete_cell` pairing.** Most findings reduce to "I freed a path but did not remove it from the list" or "I kept a pointer to a path across a stage that frees it".
5. **If the fix is in core planner code**, please share it on pgsql-hackers — this is the wider discussion pg_pathcheck is meant to feed.

## Tracking PG 17.x and 18.x point releases

The walker depends on the field names of every `Path` subtype in
`src/include/nodes/pathnodes.h`. The set of NodeTags that count as Path
subtypes is generated at build time by `gen_pathtags.pl` from whichever
`pathnodes.h` is installed, so `is_path_tag()` cannot drift out of sync —
but the walker's per-subtype switch in `walk_path()` accesses fields by
name and assumes the PG 17 / PG 18 layout.

What you should know about back-branch drift:

- **A field rename in a back-branch fix breaks the build at `walk_path()`.**
  This is the loud failure mode — fix the access knowingly, do not just
  rename the cast.
- **A field addition is silent.** If a back-branch adds a new `Path *`
  field on an already-walked struct, `walk_path()` continues to compile
  and silently narrows coverage; only an audit against `pathnodes.h` will
  catch that. Layout changes of this kind on stable branches are rare but
  not unheard of.

If a 17.x or 18.x minor release does ship such a change, audit the
affected `case` arm in `walk_path()` and update the access. There is no
re-bless workflow on this branch — the structural-hash guards used on
master were stripped because maintaining three parallel hash sets was
not worth the cost.

## Continuous coverage

The repo ships a GitHub Actions workflow (`.github/workflows/regress.yml`) that runs `make -k check-world` with `pg_pathcheck` loaded, on every push to `main`, on every PR, on manual dispatch, and nightly. Artefacts include the raw server logs and a summary written to the job's step-summary panel.

> **Note (this branch):** the workflow as shipped clones PostgreSQL master.
> On the PG 17 / 18 branch the workflow needs to be retargeted at
> `REL_17_STABLE` / `REL_18_STABLE` (or run as a matrix over both); until
> that change lands, CI on this branch will not match the source and is
> expected to fail.

## Regression tests

A small smoke test ships under `sql/pg_pathcheck.sql` with the blessed
output in `expected/pg_pathcheck.out`. It exercises GUC registration and
round-trip, the reserved-prefix mechanism (`MarkGUCPrefixReserved`), and
a sweep of SQL shapes that reach the major Path subtypes the walker
dispatches on. It does *not* test detection — see Phase 2 in the project
notes for that.

Run it against an installed cluster that has `pg_pathcheck` in
`shared_preload_libraries`:

```bash
USE_PGXS=1 PG_CONFIG=/path/to/install/bin/pg_config make installcheck
```

In-tree builds use `make check`; the Makefile passes
`--temp-config=pg_pathcheck.conf` so the temp instance preloads the
library automatically. The same `expected/` file passes on PG 17.9 and
PG 18.3.

A few SQL shapes are deliberately avoided in the test (`GROUP BY a + ORDER BY a`, `DISTINCT b + ORDER BY b`) because at the time of writing they trigger a real `UPPERREL_ORDERED` pathlist finding on PG 18 stable. The walker is doing what it should; the test avoids the trigger only because the corrupt-memory `UNDEF(N)` value varies by memory layout and would make the regression diff non-portable. See the comment at the top of `sql/pg_pathcheck.sql` for context.

## Versioning and releases

The single source of truth for the version string is `META.json`'s `"version"` field. Two derived strings must be kept in sync with it manually:

| Where                                    | What                                  |
|------------------------------------------|---------------------------------------|
| `META.json` → `"version"`                | canonical (e.g. `"0.9.1"`)            |
| `META.json` → `provides.pg_pathcheck.version` | mirror of the canonical          |
| `pg_pathcheck.c` → `#define PPC_VERSION` | matches the canonical, advertised through `PG_MODULE_MAGIC_EXT.version` on PG 18+ |

To cut a release on this branch:

1. Edit `META.json` — bump both occurrences of `"version"`.
2. Edit `pg_pathcheck.c` line 32 — bump `PPC_VERSION` to match.
3. Commit. Tag if you want (e.g. `v0.9.2-pg17-18`).
4. Run `make dist` (or `USE_PGXS=1 PG_CONFIG=... make dist`).

The `dist` target produces

```
pg_pathcheck-<version>-pg17-18.zip
```

The `-pg17-18` suffix is hard-coded in the Makefile and disambiguates this branch's release from a `master` release of the same numeric version. The archive is built via `git archive --worktree-attributes` and respects `.gitattributes` `export-ignore` rules — original artwork, internal CI reports, helper scripts, and per-branch CI patches under `fixes/` are not shipped. Typical archive size is around 100 KB.

For PGXN submission, decide at upload time how you want this branch's release to appear alongside the `master` branch's release of the same numeric version: either rename the distribution in `META.json` to `pg_pathcheck-pg17-18` (separate distribution on PGXN) or version-suffix it as `0.9.1+pg17-18` (same distribution, build-metadata variant). The Makefile target is agnostic to that choice.

## Disclaimer

Most of the code in this repository — including the extension itself, the CI workflow, the helper scripts, and portions of the documentation — was generated in collaboration with a large language model (Claude). Every change was reviewed and directed by a human before being committed, but the prose and structure are largely machine-produced. Please read the code rather than trusting the comments, and report any issues upstream.
