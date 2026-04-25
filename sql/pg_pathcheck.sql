--
-- pg_pathcheck smoke + GUC tests.
--
-- Copyright (c) 2026 Andrei Lepikhov
-- Released under the MIT License; see LICENSE in the project root.
--
-- Library is preloaded via REGRESS_OPTS=--temp-config=pg_pathcheck.conf;
-- see Makefile.  Tests verify GUC registration / round-trip and that a
-- representative SQL sweep raises no walker findings on a clean tree.
--

-- pg_pathcheck.elevel: enum membership and round-trip.
SET pg_pathcheck.elevel = 'warning';
SHOW pg_pathcheck.elevel;
SET pg_pathcheck.elevel = 'error';
SET pg_pathcheck.elevel = 'panic';
RESET pg_pathcheck.elevel;
SHOW pg_pathcheck.elevel;

-- pg_pathcheck.stage_checks: bool round-trip.
SET pg_pathcheck.stage_checks = on;
SHOW pg_pathcheck.stage_checks;
RESET pg_pathcheck.stage_checks;

-- pg_pathcheck.end_walk: bool round-trip; default is on.
SHOW pg_pathcheck.end_walk;
SET pg_pathcheck.end_walk = off;
SHOW pg_pathcheck.end_walk;
RESET pg_pathcheck.end_walk;

-- Invalid GUC values: bad enum and reserved-prefix typo.
\set VERBOSITY terse
SET pg_pathcheck.elevel = 'fatal';
SET pg_pathcheck.bogus = 1;
\set VERBOSITY default

-- Representative SQL sweep.  None of these should emit a pg_pathcheck
-- warning on a clean tree; if any do, the regression diff catches it.
--
-- Shapes that combine GROUP BY a + ORDER BY a (same column) or
-- DISTINCT b + ORDER BY b are deliberately avoided: at the time of
-- writing, PG 18 stable does in fact produce a UPPERREL_ORDERED
-- pathlist finding under those shapes (i.e. pg_pathcheck *works*),
-- but the UNDEF(N) values vary by memory layout and would make the
-- regression diff non-portable.  We wrap GROUP BY / DISTINCT in
-- single-row aggregates so the same Path subtypes still get walked.
CREATE TABLE ppc_t1(a int, b int);
CREATE TABLE ppc_t2(a int, b int);
INSERT INTO ppc_t1 SELECT i, i FROM generate_series(1, 100) i;
INSERT INTO ppc_t2 SELECT i, i FROM generate_series(1, 100) i;

-- Plain SELECT.
SELECT count(*) FROM ppc_t1;

-- Two-way join (exercises NestPath / MergePath / HashPath cases).
SELECT count(*) FROM ppc_t1 a JOIN ppc_t2 b ON a.a = b.a;

-- GROUP BY wrapped in count() (AggPath / GroupPath, deterministic output).
SELECT count(*) FROM (SELECT a FROM ppc_t1 GROUP BY a) g;

-- ORDER BY on a non-grouping expression (avoids the PG 18 corruption
-- but still exercises SortPath above an AggPath).
SELECT a, sum(b) AS s
  FROM ppc_t1 WHERE a < 6 GROUP BY a ORDER BY sum(b);

-- Window function (WindowAggPath).
SELECT a, b, lag(b) OVER (ORDER BY a)
  FROM ppc_t1 WHERE a < 6 ORDER BY a;

-- Recursive CTE (RecursiveUnionPath, non_recursive_path).
WITH RECURSIVE r(n) AS (
    SELECT 1
    UNION ALL
    SELECT n+1 FROM r WHERE n < 5
)
SELECT * FROM r;

-- INSERT ... SELECT (ModifyTablePath).
CREATE TABLE ppc_t3(a int, b int);
INSERT INTO ppc_t3 SELECT a, b FROM ppc_t1 WHERE a < 5;
SELECT count(*) FROM ppc_t3;

-- DELETE.
DELETE FROM ppc_t3 WHERE a < 3;
SELECT count(*) FROM ppc_t3;

-- DISTINCT wrapped in count() (UpperUniquePath, deterministic output).
SELECT count(*) FROM (SELECT DISTINCT b FROM ppc_t1) d;

-- UNION (SetOpPath).
SELECT a FROM ppc_t1 WHERE a < 3
UNION
SELECT a FROM ppc_t2 WHERE a < 3
ORDER BY a;

-- Stage-check tripwires: re-run a couple of representative shapes with the
-- per-stage hooks armed.  Same expected output (no warnings).
SET pg_pathcheck.stage_checks = on;
SELECT count(*) FROM ppc_t1 a JOIN ppc_t2 b ON a.a = b.a;
SELECT count(*) FROM (SELECT a FROM ppc_t1 GROUP BY a) g;
RESET pg_pathcheck.stage_checks;

-- end_walk = off: walker is silenced, query still runs.
SET pg_pathcheck.end_walk = off;
SELECT count(*) FROM ppc_t1;
RESET pg_pathcheck.end_walk;

-- elevel = error: still no error if there is no corruption.
SET pg_pathcheck.elevel = 'error';
SELECT count(*) FROM ppc_t1;
RESET pg_pathcheck.elevel;

-- Cleanup.
DROP TABLE ppc_t1, ppc_t2, ppc_t3;
