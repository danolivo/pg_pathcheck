<p align="center">
  <img src="pics/banner-readme-dark.jpg" alt="pg_pathcheck banner" />
</p>

# pg_pathcheck

PostgreSQL extension that validates planner final Path tree state, detecting freed or corrupt memory by walking every reachable Path and checking NodeTags.

## How it works

The extension uses `create_upper_paths_hook` and `planner_shutdown_hook`.
It walks the entire Path tree rooted at the top `PlannerInfo`, then
recurses into every subquery subroot reachable via `RelOptInfo::subroot`,
every subplan subroot in `PlannerGlobal::subroots`, and every parallel
RelOptInfo (`unique_rel`, `grouped_rel`, `part_rels`). For each visited
RelOptInfo it inspects `pathlist`, `partial_pathlist`,
`cheapest_parameterized_paths`, and the `cheapest_startup_path` /
`cheapest_total_path` singletons. Compound Path nodes embed further
Path pointers (`outerjoinpath`/`innerjoinpath`, `subpath`, `subpaths`,
`bitmapqual`, ...); the walker descends into each so a corrupt pointer
one level down is still caught.

Two detection mechanisms run together. The first is a NodeTag whitelist:
if `path->type` is not one of the known Path descendants, the memory has
been freed (with `CLOBBER_FREED_MEMORY` the bytes read as `0x7F`) or
reallocated for a node of a different class. The second is a
parent-match check on base/join rels: every Path found directly on
`rel`'s own lists must carry `path->parent == rel`. A mismatch catches
same-size-class aliasing where the freed slot has been recycled into
another Path that passes the tag test — the parent pointer reveals it's
been claimed by a different owner.

## Disclaimer

Most of the code in this repository — including the extension itself, the
CI workflow, the helper scripts, and portions of the documentation — was
generated in collaboration with a large language model (Claude). Every
change was reviewed and directed by a human before being committed, but
the prose and structure are largely machine-produced. Please read the
code rather than trusting the comments, and report any issues upstream.
