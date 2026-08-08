
# Copyright (c) 2026, PostgreSQL Global Development Group

#
# 001_smoke.pl
#	  Drive a query shape through every case in walk_path() and confirm
#	  pg_pathcheck survives the trip.
#
#	  This is a test *of the detector*, not of PostgreSQL.  Findings about
#	  core are expected -- that is the whole point of the module -- so the
#	  test deliberately does not fail on them; it prints them as diagnostics
#	  and leaves the verdict to a human.  What it does assert is that the
#	  walker itself stays healthy:
#
#	    - the module loads and its GUCs are visible;
#	    - no query crashes the backend (walk_path() reads a lot of memory
#	      that a corrupt tree makes unreadable, so this is not a given);
#	    - no "unhandled Path subtype" is ever reported, which is the
#	      runtime symptom of walk_path()'s switch having fallen behind
#	      pathnodes.h.  That last one is the assertion worth having: the
#	      compile-time guards can be satisfied by `make bless-path-hashes`
#	      without anyone teaching the switch about a new node.
#
#	  Both stage_checks settings are exercised, since the per-stage hooks
#	  and the end-of-planning walker reach the rels by different routes.
#

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pathcheck');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_pathcheck'

# Findings go to the server log only.  At the default 'warning' they would
# also reach the client, which is how they end up polluting regression
# diffs; here it would simply make psql output unpredictable.
pg_pathcheck.elevel = 'log'

# Keep the planner willing to produce the shapes we are trying to reach.
max_parallel_workers_per_gather = 2
min_parallel_table_scan_size = 0
min_parallel_index_scan_size = 0
parallel_setup_cost = 0
parallel_tuple_cost = 0
enable_memoize = on
jit = off
});
$node->start;

#
# The module registers no SQL objects, so the only evidence it loaded is
# that its GUCs exist.
#
my $elevel = $node->safe_psql('postgres', 'SHOW pg_pathcheck.elevel');
is($elevel, 'log', 'pg_pathcheck is loaded and its GUC is set');

my $stage = $node->safe_psql('postgres', 'SHOW pg_pathcheck.stage_checks');
is($stage, 'off', 'pg_pathcheck.stage_checks defaults to off');

#
# pg_pathcheck.elevel is PGC_SUSET because 'panic' would take down the
# whole cluster.  Confirm an unprivileged user cannot reach for it.
#
$node->safe_psql('postgres',
	q{CREATE ROLE regress_ppc_user LOGIN NOSUPERUSER});
my ($rc, $stdout, $stderr) = $node->psql('postgres',
	q{SET pg_pathcheck.elevel = 'panic'},
	extra_params => [ '-U', 'regress_ppc_user' ]);
isnt($rc, 0, 'non-superuser cannot set pg_pathcheck.elevel');
like(
	$stderr,
	qr/permission denied/,
	'... and is told why');

#
# Schema.  Deliberately varied: a partitioned table for Append /
# MergeAppend / partitionwise plans, an inheritance-free pair for the join
# shapes, and indexes so bitmap and index-only paths are reachable.
#
$node->safe_psql(
	'postgres', q{
CREATE TABLE t1 (a int, b int, c text);
CREATE TABLE t2 (a int, b int, c text);
INSERT INTO t1 SELECT i, i % 50, 'v' || i FROM generate_series(1, 2000) i;
INSERT INTO t2 SELECT i, i % 30, 'w' || i FROM generate_series(1, 2000) i;
CREATE INDEX t1_a ON t1(a);
CREATE INDEX t1_b ON t1(b);
CREATE INDEX t2_a ON t2(a);

CREATE TABLE p (a int, b int) PARTITION BY RANGE (a);
CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (1000);
CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (1000) TO (2000);
INSERT INTO p SELECT i, i % 10 FROM generate_series(0, 1999) i;
CREATE INDEX p1_a ON p1(a);
CREATE INDEX p2_a ON p2(a);

ANALYZE;
});

#
# One entry per Path subtype walk_path() knows about, so a case that stops
# being reachable is at least visible in the test list.  The label is what
# the plan is meant to contain; we do not assert on plan shape, because
# pinning EXPLAIN output would make this test a planner regression test
# and it would rot on every cost-model change.
#
my @queries = (
	[ 'Path (seq scan)'      => 'SELECT count(*) FROM t1' ],
	[ 'IndexPath'            => 'SELECT a FROM t1 WHERE a = 42' ],
	[ 'BitmapHeapPath'       => 'SELECT count(*) FROM t1 WHERE b = 3' ],
	[   'BitmapAndPath / BitmapOrPath' =>
		  'SELECT count(*) FROM t1 WHERE (a < 100 AND b = 3) OR b = 7'
	],
	[ 'TidPath'              => q{SELECT a FROM t1 WHERE ctid = '(0,1)'} ],
	[ 'TidRangePath'         => q{SELECT count(*) FROM t1 WHERE ctid < '(2,0)'} ],
	[   'SubqueryScanPath' =>
		  'SELECT count(*) FROM (SELECT a FROM t1 OFFSET 0) s'
	],
	[ 'AppendPath'           => 'SELECT count(*) FROM p' ],
	[   'MergeAppendPath' =>
		  'SELECT a FROM p ORDER BY a LIMIT 10'
	],
	[ 'GroupResultPath'      => 'SELECT 1 WHERE false' ],
	[   'MaterialPath / MemoizePath' =>
		  'SELECT count(*) FROM t1 JOIN t2 ON t1.b = t2.b'
	],
	[   'GatherPath / GatherMergePath' =>
		  'SELECT count(*) FROM (SELECT a FROM t1 ORDER BY a) s'
	],
	[   'NestPath' =>
		  'SELECT count(*) FROM t1, t2 WHERE t1.a = t2.a AND t1.a < 5'
	],
	[   'MergePath' =>
		  'SET LOCAL enable_hashjoin = off; SELECT count(*) FROM t1 JOIN t2 USING (a)'
	],
	[ 'HashPath'             => 'SELECT count(*) FROM t1 JOIN t2 USING (a)' ],
	[ 'ProjectionPath'       => 'SELECT a + 1, b * 2 FROM t1 ORDER BY c' ],
	[ 'ProjectSetPath'       => 'SELECT generate_series(1, b) FROM t1 LIMIT 20' ],
	[ 'SortPath'             => 'SELECT * FROM t1 ORDER BY c' ],
	[   'IncrementalSortPath' =>
		  'SELECT * FROM t1 ORDER BY a, c LIMIT 100'
	],
	[   'GroupPath' =>
		  'SET LOCAL enable_hashagg = off; SELECT b, count(*) FROM t1 GROUP BY b'
	],
	[ 'UniquePath'           => 'SELECT DISTINCT b FROM t1' ],
	[ 'AggPath'              => 'SELECT b, count(*) FROM t1 GROUP BY b' ],
	[   'GroupingSetsPath' =>
		  'SELECT a, b, count(*) FROM t1 GROUP BY GROUPING SETS ((a), (b), ())'
	],
	[ 'MinMaxAggPath'        => 'SELECT min(a), max(a) FROM t1' ],
	[   'WindowAggPath' =>
		  'SELECT a, row_number() OVER (PARTITION BY b ORDER BY a) FROM t1'
	],
	[   'SetOpPath' =>
		  'SELECT a FROM t1 INTERSECT SELECT a FROM t2 EXCEPT SELECT 1'
	],
	[   'RecursiveUnionPath' =>
		  'WITH RECURSIVE r(n) AS (VALUES (1) UNION ALL SELECT n + 1 FROM r WHERE n < 50) SELECT count(*) FROM r'
	],
	[ 'LockRowsPath'         => 'SELECT * FROM t1 WHERE a = 1 FOR UPDATE' ],
	[   'ModifyTablePath' =>
		  'INSERT INTO t2 SELECT a, b, c FROM t1 WHERE a < 5'
	],
	[ 'LimitPath'            => 'SELECT * FROM t1 ORDER BY a LIMIT 5 OFFSET 5' ],
	[   'subplan subroots' =>
		  'SELECT a FROM t1 WHERE b IN (SELECT b FROM t2 WHERE t2.a = t1.a) LIMIT 5'
	],
	[   'CTE and nested subqueries' =>
		  'WITH x AS MATERIALIZED (SELECT a, b FROM t1) SELECT count(*) FROM x JOIN t2 USING (a)'
	],
);

#
# Run the battery twice: the per-stage hooks reach rels that the
# end-of-planning walker sees only after grouping_planner has rearranged
# them, so the two settings exercise genuinely different code.
#
foreach my $stage_checks (qw(off on))
{
	foreach my $case (@queries)
	{
		my ($label, $sql) = @$case;

		my ($qrc, $qout, $qerr) = $node->psql(
			'postgres',
			"SET pg_pathcheck.stage_checks = $stage_checks;\n$sql",
			on_error_stop => 0);

		is($qrc, 0, "[stage_checks=$stage_checks] $label")
		  or diag("query: $sql\nstderr: $qerr");
	}
}

# EXPLAIN plans through the same hooks without executing; cheap extra cover.
$node->safe_psql('postgres',
	'EXPLAIN (COSTS OFF) SELECT count(*) FROM t1 JOIN t2 USING (a) GROUP BY t1.b');

#
# The server must still be there.  A walker that dereferences a stale
# pointer takes the backend with it, and that is exactly the failure this
# module exists to avoid inflicting.
#
is($node->safe_psql('postgres', 'SELECT 1'), '1',
	'backend survived the whole battery');

my $log = slurp_file($node->logfile);

#
# The assertion that matters.  Everything else in the log is a statement
# about PostgreSQL; this one is a statement about pg_pathcheck.
#
my @unhandled = ($log =~ /unhandled Path subtype (\w+)/g);
is(scalar(@unhandled), 0, 'walk_path() handles every Path subtype it met')
  or diag('unhandled: ' . join(', ', do { my %s; grep { !$s{$_}++ } @unhandled }));

unlike($log, qr/PANIC/, 'no PANIC in the server log');
unlike($log, qr/TRAP: failed Assert/, 'no failed assertion in the server log');

#
# Findings about core are the module working as intended, not a test
# failure.  Surface them so a human running this by hand sees them.
#
my @findings = ($log =~ /pg_pathcheck: ([^\n]*)/g);
if (@findings)
{
	my %seen;
	my @uniq = grep { !$seen{$_}++ } @findings;
	diag(sprintf('pg_pathcheck reported %d finding(s), %d distinct:',
		scalar(@findings), scalar(@uniq)));
	diag("  $_") for @uniq[ 0 .. ($#uniq > 9 ? 9 : $#uniq) ];
	diag('  ...') if @uniq > 10;
}
else
{
	diag('pg_pathcheck reported no findings against this server');
}

$node->stop;
done_testing();
