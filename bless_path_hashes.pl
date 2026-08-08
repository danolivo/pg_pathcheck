#!/usr/bin/perl
#-------------------------------------------------------------------------
#
# bless_path_hashes.pl
#	  Refresh the expected-hash X-macros in pg_pathcheck.c from the current
#	  pathtags_generated.h.
#
#	  This is a convenience for the developer who has just audited
#	  walk_path() after a layout change in core and wants to bless the
#	  new hashes without hand-copying 16-digit hex values.  The audit of
#	  walk_path() is not automated and is the whole point of the guard;
#	  use this only after confirming that walk_path() still dereferences
#	  the right fields.
#
#	  It deliberately *only* refreshes hashes for subtypes already listed.
#	  Adding a subtype means teaching walk_path() how to descend into it,
#	  and removing one means deleting its case; neither is something a
#	  script can decide, and quietly doing the bookkeeping half would let
#	  the count-parity assert be satisfied while walk_path() stayed stale.
#	  So a subtype that appeared or vanished upstream is reported as an
#	  error telling the developer what to edit by hand.
#
# usage:
#	  perl bless_path_hashes.pl <pg_pathcheck.c> <pathtags_generated.h>
#
# Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
#
#-------------------------------------------------------------------------

use strict;
use warnings FATAL => 'all';

my $src_path = shift @ARGV;
my $gen_path = shift @ARGV;

defined $src_path or die "usage: $0 <pg_pathcheck.c> <pathtags_generated.h>\n";
defined $gen_path or die "usage: $0 <pg_pathcheck.c> <pathtags_generated.h>\n";

# The X-macro blocks we maintain, in the order they appear in the source.
my @BLOCKS = qw(PPC_WALK_PATH_EXPECTED_HASHES PPC_ABSTRACT_PATH_EXPECTED_HASHES);

#
# Harvest the authoritative hash values from the generated header.  The
# captured key is exactly the token the C X-macro lists use: "T_SortPath"
# for a concrete subtype, "JoinPath" for an abstract one.
#
open(my $gh, '<', $gen_path) or die "could not open \"$gen_path\": $!";
my %hash;
while (my $line = <$gh>)
{
	if ($line =~ /^\#define \s+ PPC_PATH_HASH_(\w+) \s+ (0x[0-9a-fA-F]+ULL)/x)
	{
		$hash{$1} = $2;
	}
}
close $gh;

die "$0: no PPC_PATH_HASH_* definitions found in $gen_path\n"
	if keys %hash == 0;

# Read pg_pathcheck.c.
open(my $sh, '<', $src_path) or die "could not open \"$src_path\": $!";
my $src = do { local $/; <$sh> };
close $sh;

my %seen;		# every key we found across all blocks
my $entries = 0;

for my $block (@BLOCKS)
{
	#
	# The block is a backslash-continued #define spanning several lines.
	# Capture the anchor line, the body entries that carry a trailing
	# backslash, and the final entry that does not.
	#
	my $block_re = qr{
		(\#define \s+ \Q$block\E \s*\(X\) \s* \\ \n)   # anchor
		( (?: [ \t]* X\( [^\n]* \\ \n )* )             # body (with trailers)
		( [ \t]* X\( [^\n]* \) \n )                    # last entry, no trailer
	}x;

	unless ($src =~ $block_re)
	{
		die "$0: could not locate $block block in $src_path\n";
	}

	my $header    = $1;
	my $old_block = $2 . $3;

	# Existing entries, in the order the developer chose to keep them.
	my @order = ($old_block =~ /X\(\s*(\w+)\s*,/g);

	die "$0: $block contains no entries\n" if @order == 0;

	for my $tag (@order)
	{
		die "$0: $tag is listed in $block but no longer exists in "
		  . "$gen_path.\n"
		  . "    A Path subtype was removed upstream.  Delete its case from "
		  . "walk_path()\n"
		  . "    and its entry from $block by hand, then re-run.\n"
		  unless exists $hash{$tag};

		die "$0: $tag appears more than once in $block\n" if $seen{$tag}++;
	}

	# Align the hash column on the widest "<tag>," prefix in this block.
	my $maxprefix = 0;
	for my $tag (@order)
	{
		my $len = length("$tag,");
		$maxprefix = $len if $len > $maxprefix;
	}

	# Reassemble.  Pad with spaces only -- avoid tabs so the alignment is
	# stable regardless of the reader's tab width.
	my $new_block = '';
	for (my $i = 0; $i < @order; $i++)
	{
		my $tag     = $order[$i];
		my $prefix  = "$tag,";
		my $pad     = ' ' x ($maxprefix - length($prefix) + 1);
		my $trailer = ($i < $#order) ? ' \\' : '';
		$new_block .= "\tX($prefix$pad$hash{$tag})$trailer\n";
	}

	$src =~ s/\Q$header$old_block\E/$header$new_block/
	  or die "$0: failed to splice new $block into $src_path\n";

	$entries += scalar(@order);
}

#
# Anything upstream publishes that no block claims is a brand-new Path
# subtype.  Refuse: walk_path() needs a case for it, and inventing the
# entry here would let the build go green around a walker that silently
# skips the new node's sub-paths.
#
my @unknown = sort grep { !$seen{$_} } keys %hash;
if (@unknown)
{
	die "$0: new Path subtype(s) in $gen_path: "
	  . join(', ', @unknown) . "\n"
	  . "    Teach walk_path() how to descend into each one, then add an\n"
	  . "    entry by hand to the appropriate expected-hash block.  This\n"
	  . "    script only refreshes hashes for subtypes already listed.\n";
}

# Write back atomically: write to a temp file and rename, so a crash
# mid-write cannot leave a half-edited source file.
my $tmp = "$src_path.tmp.$$";
open(my $oh, '>', $tmp) or die "could not open \"$tmp\" for writing: $!";
print $oh $src;
close $oh;
rename($tmp, $src_path) or die "could not rename \"$tmp\" to \"$src_path\": $!";

printf "bless_path_hashes: refreshed %d entries across %d blocks in %s\n",
  $entries, scalar(@BLOCKS), $src_path;
