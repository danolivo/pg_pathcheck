#!/usr/bin/perl
#-------------------------------------------------------------------------
#
# gen_pathtags.pl
#	  Derive the set of Path-subtype NodeTags from src/include/nodes/pathnodes.h
#	  and emit an X-macro header consumed by pg_pathcheck.c.
#
#	  A struct S is a Path subtype iff S is "Path" itself, or S's first data
#	  field embeds, by value, a type that is already a Path subtype.  Structs
#	  marked with pg_node_attr(abstract) participate in inheritance but do not
#	  receive their own NodeTag (gen_node_support.pl does the same upstream),
#	  so we skip emitting X(T_S) for them.
#
#	  In addition to PATH_TAG_LIST, we emit a structural hash
#	  PPC_PATH_HASH_T_<Subtype> for every concrete Path subtype.  The hash
#	  is derived from the struct's body after comments, pg_node_attr(...)
#	  invocations and redundant whitespace have been stripped, so cosmetic
#	  edits do not fire the guard.  Any field addition, removal, rename,
#	  type change or reordering does.
#
#	  Abstract Path subtypes get the same treatment through
#	  PATH_ABSTRACT_LIST(X) / PPC_PATH_HASH_<Subtype> (no T_ prefix, because
#	  there is no such NodeTag).  These matter because walk_path() casts a
#	  concrete node to its abstract parent to reach inherited fields --
#	  ((JoinPath *) path)->outerjoinpath, say.  A concrete subtype's hash
#	  covers only the text "JoinPath jpath;", which does not change when
#	  JoinPath's own body does, so without these the inherited accesses
#	  would be unguarded.
#
# usage:
#	  perl gen_pathtags.pl <path-to-pathnodes.h> <output-header>
#
# Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
#-------------------------------------------------------------------------

use strict;
use warnings FATAL => 'all';
use Digest::MD5 qw(md5_hex);

my $input  = shift @ARGV;
my $output = shift @ARGV;

defined $input  or die "usage: $0 <pathnodes.h> <output-header>\n";
defined $output or die "usage: $0 <pathnodes.h> <output-header>\n";

open(my $in, '<', $input) or die "could not open \"$input\": $!";
my $src = do { local $/; <$in> };
close $in;

# Strip C comments; they can hide matching tokens in our regexes.
$src =~ s{/\*.*?\*/}{}gs;
$src =~ s{//[^\n]*}{}g;

# Walk every "typedef struct Name { ... } [Alias];".  We parse the body to
# (a) record whether the struct is abstract, and (b) find the first data
# field's type so we can climb the inheritance chain.
my %first_field_type;
my %is_abstract;
my %body_hash;
my @decl_order;

#
# One pg_node_attr(...) invocation, tolerating a single level of nested parens
# (e.g. pg_node_attr(array_size(n))).  The quantifiers are possessive so the
# pattern cannot backtrack: the naive (?:[^()]*|\([^()]*\))* spelling is an
# alternation of possibly-empty branches under a star, which degrades to
# exponential backtracking on a subject that fails to match.
#
my $attr_re = qr/pg_node_attr \s* \( (?: [^()]++ | \( [^()]*+ \) )*+ \)/x;

while ($src =~ /typedef \s+ struct \s+ (\w+) \s* \{ ( (?: [^{}]++ | \{[^{}]*\} )* ) \}/gxs)
{
	my ($name, $body) = ($1, $2);

	# Remember pg_node_attr(abstract) before we strip annotations away.  Only
	# the struct-level annotation counts, and that one precedes every field,
	# hence every semicolon; restricting the search that way keeps a
	# hypothetical field-level attribute mentioning "abstract" from
	# misclassifying the struct.
	my ($prelude) = $body =~ /\A ( [^;]* )/xs;
	$is_abstract{$name} = 1
		if defined $prelude
		&& $prelude =~ /($attr_re)/
		&& $1 =~ /\b abstract \b/x;

	# Remove every pg_node_attr(...) invocation (struct-level and trailing
	# field-level), so that split-by-semicolon yields clean field declarations
	# and so that annotation changes do not perturb the structural hash.
	1 while $body =~ s/$attr_re//g;

	# Canonical form for hashing: all whitespace collapsed to single spaces.
	# Comments are already stripped at file level; pg_node_attr invocations
	# were just removed.  What remains is the field layout, which is exactly
	# what walk_path() makes assumptions about.
	my $canonical = $body;
	$canonical =~ s/\s+/ /g;
	$canonical =~ s/^\s+|\s+$//g;
	$body_hash{$name} = substr(md5_hex($canonical), 0, 16);

	my $first_type;
	for my $field (split /;/, $body)
	{
		$field =~ s/^\s+|\s+$//g;
		next if $field eq '';

		# Skip the NodeTag header on the root of an inheritance chain;
		# we want the first *substantive* struct field.
		next if $field =~ /^NodeTag\s+type\b/;

		#
		# Inheritance in the Node tree is spelled by embedding the parent
		# struct *by value* as the first substantive field, so accept only
		# the plain "TypeName fieldname" form here -- no pointer, no array.
		# A struct whose leading field is "Path *something" is not a Path
		# subtype, and treating it as one would inject a bogus tag into
		# PATH_TAG_LIST and thereby widen is_path_tag()'s whitelist, which
		# is precisely the check we cannot afford to weaken.
		#
		# Note we examine the first substantive field and then stop: if it
		# is not an embedded struct, this type inherits from nothing.
		#
		$first_type = $1
			if $field =~ /^([A-Za-z_]\w*)\s+[A-Za-z_]\w*$/;
		last;
	}

	$first_field_type{$name} = $first_type;
	push @decl_order, $name;
}

# Transitive classification: seed with Path, iterate until fixed point.
my %is_path = (Path => 1);
my $changed = 1;
while ($changed)
{
	$changed = 0;
	for my $name (@decl_order)
	{
		next if $is_path{$name};
		my $parent = $first_field_type{$name};
		if (defined $parent && $is_path{$parent})
		{
			$is_path{$name} = 1;
			$changed = 1;
		}
	}
}

# Split Path subtypes into concrete (tagged) and abstract (inherited-from).
my @emitted  = grep { $is_path{$_} && !$is_abstract{$_} } @decl_order;
my @abstract = grep { $is_path{$_} && $is_abstract{$_} } @decl_order;

die "$0: no Path subtypes detected in $input — parser broken?\n"
	if @emitted == 0;

open(my $out, '>', $output) or die "could not open \"$output\" for writing: $!";

print $out <<"EOT";
/*-------------------------------------------------------------------------
 *
 * pathtags_generated.h
 *	  Auto-generated descriptor of every concrete Path subtype in
 *	  src/include/nodes/pathnodes.h.  DO NOT EDIT BY HAND — regenerated by
 *	  contrib/pg_pathcheck/gen_pathtags.pl on every build.
 *
 *	  PATH_TAG_LIST(X) expands X(T_<Subtype>) in declaration order.
 *	  Typical use:
 *
 *		switch (tag)
 *		{
 *	#define PPC_X(t) case t:
 *			PATH_TAG_LIST(PPC_X)
 *	#undef PPC_X
 *				return true;
 *			default:
 *				return false;
 *		}
 *
 *	  PATH_ABSTRACT_LIST(X) expands X(<Subtype>) — no T_ prefix — for the
 *	  abstract Path subtypes, i.e. the ones that carry a NodeTag only
 *	  through their concrete descendants.
 *
 *	  In addition, PPC_PATH_HASH_T_<Subtype> (concrete) and
 *	  PPC_PATH_HASH_<Subtype> (abstract) give a 64-bit structural hash of
 *	  that subtype's body — stable under cosmetic edits, changes on any
 *	  real field addition/removal/rename/type-change/reorder.
 *	  pg_pathcheck.c cross-checks these against a hand-blessed mirror so
 *	  an upstream layout change fails the build before walk_path() can
 *	  silently miss a new sub-path field.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHTAGS_GENERATED_H
#define PATHTAGS_GENERATED_H

#define PATH_TAG_LIST(X) \\
EOT

for (my $i = 0; $i < @emitted; $i++)
{
	my $trailer = ($i < $#emitted) ? ' \\' : '';
	print $out "\tX(T_$emitted[$i])$trailer\n";
}

print $out "\n#define PATH_ABSTRACT_LIST(X)";
for (my $i = 0; $i < @abstract; $i++)
{
	my $trailer = ($i < $#abstract) ? ' \\' : '';
	print $out " \\\n\tX($abstract[$i])$trailer";
}
print $out "\n\n";

# Map each subtype to the macro name its hash is published under, then pad
# to the longest so the constants line up in the generated header.
my %symbol;
$symbol{$_} = "PPC_PATH_HASH_T_$_" for @emitted;
$symbol{$_} = "PPC_PATH_HASH_$_"   for @abstract;

my $maxlen = 0;
for my $name (@emitted, @abstract)
{
	my $len = length($symbol{$name});
	$maxlen = $len if $len > $maxlen;
}

for my $name (@emitted, @abstract)
{
	my $pad = ' ' x ($maxlen - length($symbol{$name}));
	print $out "#define $symbol{$name}$pad 0x$body_hash{$name}ULL\n";
}

print $out "\n#endif\t\t\t\t\t\t\t/* PATHTAGS_GENERATED_H */\n";

close $out;
