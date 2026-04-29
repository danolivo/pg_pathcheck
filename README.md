<p align="center">
  <img src="pics/banner-readme-dark.jpg" alt="pg_pathcheck banner" />
</p>

# pg_pathcheck

PostgreSQL extension that validates the planner's Path tree, detecting freed or corrupt memory by walking every reachable Path and checking NodeTags.
Reports and analyses of findings are published on the [project wiki](https://github.com/danolivo/pg_pathcheck/wiki).

## Which branch do I want?

`pg_pathcheck` lives on more than one long-running Git branch. Each branch targets a different PostgreSQL release line; the **user-visible interface (name, GUCs, warning format) is identical across branches** so findings are directly comparable.

| If you run                          | Check out branch                                                                  | Tracks upstream                |
|-------------------------------------|-----------------------------------------------------------------------------------|--------------------------------|
| PostgreSQL **17.x** or **18.x**     | [`pg17-18`](https://github.com/danolivo/pg_pathcheck/tree/pg17-18)                | `REL_17_STABLE`, `REL_18_STABLE` |
| PostgreSQL **master / 19devel**     | [`main`](https://github.com/danolivo/pg_pathcheck/tree/main)                      | `master`                       |

The implementation diverges from `pg17-18` because the available planner hooks differ between PG versions: `planner_shutdown_hook` and the `extension_state` slot API used here are master-only additions (see the patch in `fixes/`). On master the walker runs in the dedicated shutdown hook after `standard_planner` returns; on 17/18 it is driven inline from `create_upper_paths_hook` at `UPPERREL_FINAL`.

**Coverage caveat between branches.** Because the 17/18 walk fires before `create_plan` and `setrefs.c`, any Path-lifetime bug visible only during those later stages is caught on `main` but not on `pg17-18`. Base-, join- and upper-rel Path generation is complete by `UPPERREL_FINAL` so the bulk of the detection surface is the same on both branches.

## How it works

The extension uses `create_upper_paths_hook` and `planner_shutdown_hook`. It walks the entire Path tree rooted at the top `PlannerInfo`, then recurses into every subquery subroot reachable via `RelOptInfo::subroot`, every subplan subroot in `PlannerGlobal::subroots`, and every parallel RelOptInfo (`unique_rel`, `grouped_rel`, `part_rels`). For each visited RelOptInfo, it inspects `pathlist`, `partial_pathlist`, `cheapest_parameterized_paths`, and the `cheapest_startup_path` / `cheapest_total_path` singletons. Compound Path nodes embed further Path pointers (`outerjoinpath`/`innerjoinpath`, `subpath`, `subpaths`, `bitmapqual`, ...); the walker descends into each, so a corrupt pointer one level down is still caught.

Two detection mechanisms run together. The first is a NodeTag whitelist: if `path->type` is not one of the known Path-family node tags, the memory has been freed (with `CLOBBER_FREED_MEMORY` the bytes read as `0x7F`) or reallocated for a node of a different class. The second is a parent-match check on base and join rels: every Path found directly on the rel's own lists must carry `path->parent == rel`. A mismatch catches same-size-class aliasing, where the freed slot has been recycled into another Path that passes the tag test — the parent pointer reveals the slot has been claimed by a different owner.

## Installing

The `pg_pathcheck` targets **PostgreSQL master**. Since it is a pure module without any UI registered in the database, it uses `PG_MODULE_MAGIC_EXT` to expose the code version and the `extension_state` slot API. For better results, use a **cassert build** so that `CLOBBER_FREED_MEMORY` is enabled. A typical configure line:

```bash
./configure --prefix=/path/to/install \
    --enable-cassert --enable-debug
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

The release tarball is `pg_pathcheck-<version>.zip`, produced by `make dist`; see [Versioning and releases](#versioning-and-releases) for how it is built.

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
SET pg_pathcheck.elevel = 'warning';  -- default, complain and keep going
SET pg_pathcheck.elevel = 'log';  -- log and keep going
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

Turning `stage_checks` on adds three extra hooks that fire during planning:

| Hook | Fires at | Pins a finding to |
|---|---|---|
| `set_rel_pathlist_hook` | end of `set_*_pathlist()` for each base rel | base-rel path generation |
| `set_join_pathlist_hook` | end of `populate_joinrel_with_paths()` | join-rel construction |
| `create_upper_paths_hook` | end of each `UPPERREL_*` stage | a specific upper-rel stage (ordering, grouping, distinct, …) |

Each hook checks every entry in the affected rel's `pathlist` / `partial_pathlist` for a valid Path NodeTag and — for base/join rels — a matching `->parent`. A finding carries a short context string (`"base rel"`, `"outer side of join rel {a,b}"`, `"create_upper_paths input, stage UPPERREL_ORDERED"`) that identifies which hook caught it.

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

## Bumping PostgreSQL

The walker depends on the layout of every `Path` subtype in
`src/include/nodes/pathnodes.h`. When you pull a new PostgreSQL master and
that header has moved, the build catches the drift before it turns into a
runtime `Assert(false)` in `walk_path()`.

There are two compile-time guards in `pg_pathcheck.c`:

- **Subtype-set guard.** `PPC_WALK_PATH_EXPECTED_HASHES` must list exactly
  the same set of concrete Path subtypes as `PATH_TAG_LIST` (generated from
  `pathnodes.h`). If core adds or removes a subtype, a `static_assert`
  fires naming the mismatch.

- **Layout guard.** Each entry in `PPC_WALK_PATH_EXPECTED_HASHES` carries a
  64-bit structural hash of its subtype's body (comments,
  `pg_node_attr(...)` and redundant whitespace stripped before hashing).
  If core edits any walked struct, a `static_assert` fires naming the
  specific subtype.

### Re-bless workflow

1. Rebuild. The failing assertion tells you whether the subtype *set* or a
   specific subtype's *layout* drifted, and which subtype.
2. Read the commit in upstream that touched `pathnodes.h`.
3. **Audit `walk_path()`**. For each drifted subtype, confirm that the
   field accesses in its `case` still reach the same sub-paths. Add cases
   for new subtypes; remove cases for deleted ones. Teach `walk_path()`
   about any new `Path *` or path-list fields.
4. Run `make bless-path-hashes`. This rewrites the
   `PPC_WALK_PATH_EXPECTED_HASHES` block in `pg_pathcheck.c` with the
   current hashes. **Do not run this without doing step 3 first** — it
   defeats the entire guard.
5. Rebuild. The build should be green again.

### Scope and limitations

- The layout hash covers only a subtype's own body. Embedded parent
  structs (`JoinPath` inside `NestPath`, `SortPath` inside
  `IncrementalSortPath`) are not hashed transitively; they rely on their
  own independent hash entries. Today every parent in the chain is
  independently guarded, so this is sound — but if future walker code
  dereferences a non-`Path` struct embedded in a Path, that struct's
  layout changes will *not* trip the guard and must be handled
  separately.
- Cosmetic edits in `pathnodes.h` (comment rewrap, `pg_node_attr`
  annotation changes, whitespace) are filtered out of the hash, so they
  do not force a re-bless.
- Field reordering that preserves the field set *does* force a re-bless
  even though the walker is typically unaffected. Accepted cost.

## Continuous coverage

The repo ships a GitHub Actions workflow (`.github/workflows/regress.yml`) that runs `make -k check-world` with `pg_pathcheck` loaded on every push to `main`, on every PR, on manual dispatch, and nightly. Artefacts include the raw server logs and a summary written to the job's step-summary panel. Point your fork or extension repo at the same workflow if you would like continuous coverage against PG master as it evolves.

## Versioning and releases

The single source of truth for the version string is `META.json`'s `"version"` field. Two derived strings must be kept in sync with it manually:

| Where                                         | What                                                                              |
|-----------------------------------------------|-----------------------------------------------------------------------------------|
| `META.json` → `"version"`                     | canonical (e.g. `"0.9"`)                                                          |
| `META.json` → `provides.pg_pathcheck.version` | mirror of the canonical                                                           |
| `pg_pathcheck.c` → `#define PPC_VERSION`      | matches the canonical, advertised through `PG_MODULE_MAGIC_EXT.version`           |

To cut a release on this branch:

1. Edit `META.json` — bump both occurrences of `"version"`.
2. Edit `pg_pathcheck.c` — bump `PPC_VERSION` to match.
3. Commit. Tag if you want (e.g. `v0.9.1`).
4. Run `make dist` (or `USE_PGXS=1 PG_CONFIG=... make dist`).

The `dist` target produces

```
pg_pathcheck-<version>.zip
```

This is the master-targeted distribution and carries the canonical numeric version with no branch suffix; the parallel `pg17-18` release adds a `-pg17-18` suffix to its archive filename to disambiguate. The archive is built via `git archive --worktree-attributes` and respects `.gitattributes` `export-ignore` rules — original artwork, internal CI reports, helper scripts, and per-branch CI patches under `fixes/` are not shipped. Typical archive size is around 100 KB.

## Disclaimer

Most of the code in this repository — including the extension itself, the CI workflow, the helper scripts, and portions of the documentation — was generated in collaboration with a large language model (Claude). Every change was reviewed and directed by a human before being committed, but the prose and structure are largely machine-produced. Please read the code rather than trusting the comments, and report any issues upstream.
