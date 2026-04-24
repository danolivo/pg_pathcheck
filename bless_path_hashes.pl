#!/usr/bin/perl
#-------------------------------------------------------------------------
#
# bless_path_hashes.pl
#	  Rewrite the PPC_WALK_PATH_EXPECTED_HASHES X-macro in pg_pathcheck.c
#	  with the current hashes from pathtags_generated.h.
#
#	  This is a convenience for the developer who has just audited
#	  walk_path() after a layout change in core and wants to bless the
#	  new hashes without hand-copying 16-digit hex values.  The audit of
#	  walk_path() is not automated and is the whole point of the guard;
#	  use this only after confirming that walk_path() still dereferences
#	  the right fields.
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

# Harvest the authoritative hash values from the generated header.
open(my $gh, '<', $gen_path) or die "could not open \"$gen_path\": $!";
my %hash;
my @order;
while (my $line = <$gh>)
{
	# "#define PPC_PATH_HASH_T_<Subtype>  0x<hex>ULL"
	if ($line =~ /^\#define \s+ PPC_PATH_HASH_(T_\w+) \s+ (0x[0-9a-fA-F]+ULL)/x)
	{
		$hash{$1} = $2;
		push @order, $1;
	}
}
close $gh;

die "$0: no PPC_PATH_HASH_* definitions found in $gen_path\n"
	if @order == 0;

# Read pg_pathcheck.c and locate the PPC_WALK_PATH_EXPECTED_HASHES block.
open(my $sh, '<', $src_path) or die "could not open \"$src_path\": $!";
my $src = do { local $/; <$sh> };
close $sh;

# The block is a backslash-continued #define spanning many lines.  Match
# greedily up to the final entry (the line with no trailing backslash).
my $block_re = qr{
	(\#define \s+ PPC_WALK_PATH_EXPECTED_HASHES \s*\(X\) \s* \\ \n)   # anchor
	( (?: \s* X\( [^\n]* \\ \n )+ )                                   # body (with trailers)
	( \s* X\( [^\n]* \) \n )                                          # last entry, no trailer
}x;

unless ($src =~ $block_re)
{
	die "$0: could not locate PPC_WALK_PATH_EXPECTED_HASHES block in $src_path\n";
}

my $header	  = $1;
my $old_block = $2 . $3;

# Determine the widest "<tag>," prefix so the hash column lines up.
my $maxprefix = 0;
for my $tag (@order)
{
	my $prefix = "$tag,";
	$maxprefix = length($prefix) if length($prefix) > $maxprefix;
}

# Reassemble the block.  Pad with spaces only -- avoid tabs so the
# alignment is stable regardless of the reader's tab width.
my $new_body = '';
for (my $i = 0; $i < @order; $i++)
{
	my $tag		= $order[$i];
	my $prefix	= "$tag,";
	my $pad		= ' ' x ($maxprefix - length($prefix) + 1);
	my $trailer = ($i < $#order) ? ' \\' : '';
	$new_body .= "\tX($prefix$pad$hash{$tag})$trailer\n";
}

my $new_block = $new_body;

my $replaced = 0;
$src =~ s/\Q$header$old_block\E/$header$new_block/
	and $replaced = 1;

die "$0: failed to splice new block into $src_path\n"
	unless $replaced;

# Write back atomically: write to a temp file and rename, so a crash
# mid-write cannot leave a half-edited source file.
my $tmp = "$src_path.tmp.$$";
open(my $oh, '>', $tmp) or die "could not open \"$tmp\" for writing: $!";
print $oh $src;
close $oh;
rename($tmp, $src_path) or die "could not rename \"$tmp\" to \"$src_path\": $!";

printf "bless_path_hashes: refreshed %d entries in %s\n",
	scalar(@order), $src_path;
