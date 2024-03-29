#!/usr/bin/perl -w
###############################################################################
# Copyright (c) 2017 Linaro Limited
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the Eclipse Public License v1.0
# which accompanies this distribution, and is available at
# http://www.eclipse.org/legal/epl-v10.html
#
# Contributors:
#     Peter Maydell (Linaro) - initial implementation
###############################################################################

# risugen_common -- common utility routines for CPU modules.
# We don't declare ourselves in a package so all these functions
# and variables are available for use.

package risugen_common;

use strict;
use warnings;

BEGIN {
    require Exporter;

    our @ISA = qw(Exporter);
    our @EXPORT = qw(
        irand
        open_bin close_bin
        set_endian big_endian
        insn32 insn16 $bytecount
        progress_start progress_update progress_end progress_show
        eval_with_fields is_pow_of_2 sextract ctz
        update_insn
        $current_insn
        $current_rec
        dump_insn_details
        $OP_COMPARE
        $OP_TESTEND
        $OP_SETMEMBLOCK
        $OP_GETMEMBLOCK
        $OP_COMPAREMEM
        $OP_SETUPBEGIN
        $OP_SETUPEND
    );
}

our $OP_COMPARE = 0;        # compare registers
our $OP_TESTEND = 1;        # end of test, stop
our $OP_SETMEMBLOCK = 2;    # set memory block
our $OP_GETMEMBLOCK = 3;    # get the address of memory block
our $OP_COMPAREMEM = 4;     # compare memory block
our $OP_SETUPBEGIN = 5;     # setup instructions are going to be executed
our $OP_SETUPEND = 6;       # no more setup instructions

our $bytecount;

my $bigendian = 0;

# Return a random number between 0 and $ inclusive
sub irand($)
{
    my ($max) = @_;
    return int rand($max + 1);
}

# Set the endianness when insn32() and insn16() write to the output
# (default is little endian, 0).
sub set_endian
{
    $bigendian = @_;
}

sub big_endian
{
    return $bigendian;
}

sub open_bin
{
    my ($fname) = @_;
    open(BIN, ">", $fname) or die "can't open %fname: $!";
    $bytecount = 0;
}

sub close_bin
{
    close(BIN) or die "can't close output file: $!";
}

sub insn32($)
{
    my ($insn) = @_;
    print BIN pack($bigendian ? "N" : "V", $insn);
    $bytecount += 4;
}

sub insn16($)
{
    my ($insn) = @_;
    print BIN pack($bigendian ? "n" : "v", $insn);
    $bytecount += 2;
}

# Progress bar implementation
my $lastprog;
my $proglen;
my $progmax;
my $progress = 0;

sub progress_show()
{
    $progress = 1;
}

sub progress_start($$)
{
    if ($progress) {
        ($proglen, $progmax) = @_;
        $proglen -= 2; # allow for [] chars
        $| = 1;        # disable buffering so we can see the meter...
        print "[" . " " x $proglen . "]\r";
        $lastprog = 0;
    }
}

sub progress_update($)
{
    if ($progress) {
        # update the progress bar with current progress
        my ($done) = @_;
        my $barlen = int($proglen * $done / $progmax);
        if ($barlen != $lastprog) {
            $lastprog = $barlen;
            print "[" . "-" x $barlen . " " x ($proglen - $barlen) . "]\r";
        }
    }
}

sub progress_end()
{
    if ($progress) {
        print "[" . "-" x $proglen . "]\n";
        $| = 0;
    }
}

our $current_insn;
our $current_rec;

sub update_insn($$) {
    my ($changevar, $newval) = @_;
    for my $tuple (@{ $current_rec->{fields} }) {
        my ($var, $pos, $mask) = @$tuple;
        if ($var eq $changevar) {
            $$current_insn = ($$current_insn & ~($mask << $pos)) | (($newval & $mask) << $pos);
            last;
        }
    }
}

sub eval_with_fields($$$$$) {
    # Evaluate the given block in an environment with Perl variables
    # set corresponding to the variable fields for the insn.
    # Return the result of the eval; we die with a useful error
    # message in case of syntax error.
    #
    # At the moment we just evaluate the string in the environment
    # of the calling package.
    # What we *ought* to do here is to give the config snippets
    # their own package, and explicitly import into it only the
    # functions that we want to be accessible to the config.
    # That would provide better separation and an explicitly set up
    # environment that doesn't allow config file code to accidentally
    # change state it shouldn't have access to, and avoid the need to
    # use 'caller' to get the package name of our calling function.
    my ($insnname, $insn, $rec, $blockname, $block) = @_;
    $current_insn = $insn;
    $current_rec = $rec;
    my $calling_package = caller;
    my $evalstr = "{ package $calling_package; ";
    for my $tuple (@{ $rec->{fields} }) {
        my ($var, $pos, $mask) = @$tuple;
        my $val = ($$insn >> $pos) & $mask;
        $evalstr .= "my (\$$var) = $val; ";
    }
    $evalstr .= $block;
    $evalstr .= "}";
    my $v = eval $evalstr;
    if ($@) {
        print "Syntax error detected evaluating $insnname $blockname string:\n$block\n$@";
        exit(1);
    }
    return $v;
}

sub is_pow_of_2($)
{
    my ($x) = @_;
    return ($x > 0) && (($x & ($x - 1)) == 0);
}

# sign-extract from a nbit optionally signed bitfield
sub sextract($$)
{
    my ($field, $nbits) = @_;

    my $sign = $field & (1 << ($nbits - 1));
    return -$sign + ($field ^ $sign);
}

sub ctz($)
{
    # Count trailing zeros, similar semantic to gcc builtin:
    # undefined return value if input is zero.
    my ($in) = @_;

    # XXX should use log2, popcount, ...
    my $imm = 0;
    for (my $cnt = $in; $cnt > 1; $cnt >>= 1) {
        $imm += 1;
    }
    return $imm;
}

sub dump_insn_details($$)
{
    # Dump the instruction details for one insn
    my ($insn, $rec) = @_;
    print "insn $insn: ";
    my $insnwidth = $rec->{width};
    my $fixedbits = $rec->{fixedbits};
    my $fixedbitmask = $rec->{fixedbitmask};
    my $constraint = $rec->{blocks}{"constraints"};
    print sprintf(" insnwidth %d fixedbits %08x mask %08x ", $insnwidth, $fixedbits, $fixedbitmask);
    if (defined $constraint) {
        print "constraint $constraint ";
    }
    for my $tuple (@{ $rec->{fields} }) {
        my ($var, $pos, $mask) = @$tuple;
        print "($var, $pos, " . sprintf("%08x", $mask) . ") ";
    }
    print "\n";
}

1;
