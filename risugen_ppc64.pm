#!/usr/bin/perl -w
###############################################################################
# Copyright (c) IBM Corp, 2016
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the Eclipse Public License v1.0
# which accompanies this distribution, and is available at
# http://www.eclipse.org/legal/epl-v10.html
#
# Contributors:
#     Jose Ricardo Ziviani (IBM) - ppc64 implementation
#     based on Peter Maydell (Linaro) - initial implementation
###############################################################################

# risugen -- generate a test binary file for use with risu
# See 'risugen --help' for usage information.
package risugen_ppc64;

use strict;
use warnings;

use risugen_common;

require Exporter;

our @ISA    = qw(Exporter);
our @EXPORT = qw(write_test_code);

my $periodic_reg_random = 1;

#
# Maximum alignment restriction permitted for a memory op.
my $MAXALIGN = 64;

my $num = 0;

sub set_num($)
{
    my ($n) = @_;
    $num = $n;
}

sub get_num()
{
    return $num;
}

sub write_mov_ri16($$)
{
    my ($rd, $imm) = @_;

    # li rd,immediate = addi rd, 0, $imm ; includes sign extension
    insn32(0xe << 26 | $rd << 21 | ($imm & 0xffff));
}

sub write_mov_ri32($$)
{
    my ($rd, $imm) = @_;

    # lis rd,immediate@h
    insn32(0xf << 26 | $rd << 21 | (($imm >> 16) & 0xffff));
    # ori rd,rd,immediate@l
    insn32((0x18 << 26) | ($rd << 21) | ($rd << 16) | ($imm & 0xffff));
}

sub write_add_ri($$$)
{
    my ($rt, $ra, $imm) = @_;

    # addi rt, ra, immd
    insn32((0xe << 26) | ($rt << 21) | ($ra << 16) | ($imm & 0xffff));
}

sub write_mov_ri($$)
{
    my ($rd, $imm) = @_;

    if ((($imm & 0xffff8000) != 0) && (($imm & 0xffff8000) != 0xffff8000)) {
        write_mov_ri32($rd, $imm);
    } else {
        write_mov_ri16($rd, $imm);
    }
}

sub write_store_32($$)
{
    my ($im, $imm) = @_;

    write_mov_ri(20, $im);
    # stw r20, $imm+0(r1)
    insn32((0x24 << 26) | (20 << 21) | (1 << 16) | ($imm & 0xffff));
}

sub write_store_64($$$)
{
    my ($imh, $iml, $imm) = @_;

    if (big_endian()) {
        write_store_32($imh, $imm + 0);
        write_store_32($iml, $imm + 4);
    } else {
        write_store_32($iml, $imm + 0);
        write_store_32($imh, $imm + 4);
    }
}

sub write_store_128($$$$$)
{
    my ($imhh, $imh, $iml, $imll, $imm) = @_;

    if (big_endian()) {
        write_store_64($imhh, $imh, $imm + 0);
        write_store_64($iml, $imll, $imm + 8);
    } else {
        write_store_64($iml, $imll, $imm + 0);
        write_store_64($imhh, $imh, $imm + 8);
    }
}

sub write_random_ppc64_fpdata()
{
    for (my $i = 0; $i < 32; $i++) {
        # store a random doubleword value at r1+16
        write_store_64(irand(0xfffff), irand(0xfffff), 0x10);
        # lfd f$i, 0x10(r1)
        insn32((0x32 << 26) | ($i << 21) | (0x1 << 16) | 0x10);
    }
}

sub write_random_ppc64_vrdata()
{
    for (my $i = 0; $i < 32; $i++) {
        # store a random doubleword value at r1+16
        write_store_128(irand(0xffff), irand(0xffff), irand(0xfffff), irand(0xfffff), 0x10);
        # li r0, 16
        write_mov_ri(0, 0x10);
        # lvx vr$i, r1, r0
        insn32((0x1f << 26) | ($i << 21) | (0x1 << 16) | 0x2ce);
    }
}

sub write_random_regdata()
{
    # clear condition register
    for (my $i = 0; $i < 32; $i++) {
        # crxor i, i, i ; crclr i
        insn32((0x13 << 26) | ($i << 21) | ($i << 16) | ($i << 11) | (0xc1 << 1) | 0);
    }

    # general purpose registers
    for (my $i = 0; $i < 32; $i++) {
        if ($i == 1 || $i == 13) {
            next;
        }
        write_mov_ri($i, irand(0xffffffff));
    }
}

sub clear_vr_registers()
{
    # li r23, 0
    write_mov_ri(23, 0);
    # mtxer r23 ; zero the xer register
    insn32((31 << 26) | (23 << 21) | ((1 & 31) << 16) | ((1 >> 5) << 11) | (467 << 1));
    #li r23, -1
    write_mov_ri(23, -1);
    # mtspr vrsave, r23
    insn32((31 << 26) | (23 << 21) | ((256 & 31) << 16) | ((256 >> 5) << 11) | (467 << 1));

    for (my $i = 0; $i < 32; $i++) {
        # vxor vi, vi, vi
        insn32((0x4 << 26) | ($i << 21) | ($i << 16) | ($i << 11) | 0x4c4);
    }
}

sub write_random_register_data($)
{
    my ($fp_enabled) = @_;

    clear_vr_registers();

    write_random_ppc64_vrdata();
    if ($fp_enabled) {
        # load floating point / SIMD registers
        write_random_ppc64_fpdata();
    }

    write_random_regdata();
}

sub write_memblock_setup()
{
    # li r2, 20000
    write_mov_ri(2, 20000);
    # mtctr r2
    insn32(0x7c4903a6);
    # li r3, 0
    write_mov_ri(3, 0);
    # addi r2,r1,132
    insn32(0x38410084);
    # stwu r3,-4(r2)
    insn32(0x9462fffc);
    # bdnz -4
    insn32(0x4200fffc);
}

# Functions used in memory blocks to handle addressing modes.
# These all have the same basic API: they get called with parameters
# corresponding to the interesting fields of the instruction,
# and should generate code to set up the base register to be
# valid. They must return the register number of the base register.
# The last (array) parameter lists the registers which are trashed
# by the instruction (ie which are the targets of the load).
# This is used to avoid problems when the base reg is a load target.

# Global used to communicate between align(x) and reg() etc.
my $alignment_restriction;

sub align($)
{
    my ($a) = @_;
    if (!is_pow_of_2($a) || ($a < 0) || ($a > $MAXALIGN)) {
        die "bad align() value $a\n";
    }
    $alignment_restriction = $a;
}

sub get_offset()
{
    # Emit code to get a random offset within the memory block, of the
    # right alignment, into r0
    # We require the offset to not be within 256 bytes of either
    # end, to (more than) allow for the worst case data transfer, which is
    # 16 * 64 bit regs
    my $offset = (irand(2048 - 512) + 256) & ~($alignment_restriction - 1);
    return $offset
}

sub reg($@)
{
    my ($base, @trashed) = @_;
    # Now r0 is the address we want to do the access to,
    # so just move it into the basereg
    write_add_ri($base, 1, get_offset());
    if (grep $_ == $base, @trashed) {
        return -1;
    }
    return $base;
}

sub reg_plus_reg($$@)
{
    my ($ra, $rb, @trashed) = @_;

    # addi $ra, r1, 0
    write_add_ri($ra, 1, 0);
    # li $rb, 32
    write_mov_ri($rb, 32);

    return $ra
}

sub reg_plus_imm($$@)
{
    # Handle reg + immediate addressing mode
    my ($base, $imm, @trashed) = @_;

    if ($imm < 0) {
        return $base;
    }

    $imm = -$imm;
    write_add_ri($base, 1, $imm);

    # Clear r0 to avoid register compare mismatches
    # when the memory block location differs between machines.
    # write_mov_ri($base, 0);

    if (grep $_ == $base, @trashed) {
        return -1;
    }
    return $base;
}

sub gen_one_insn($$)
{
    # Given an instruction-details array, generate an instruction
    my $constraintfailures = 0;

    INSN: while(1) {
        my ($forcecond, $rec) = @_;
        my $insn = irand(0xffffffff);
        my $insnname = $rec->{name};
        my $insnwidth = $rec->{width};
        my $fixedbits = $rec->{fixedbits};
        my $fixedbitmask = $rec->{fixedbitmask};
        my $constraint = $rec->{blocks}{"constraints"};
        my $memblock = $rec->{blocks}{"memory"};

        $insn &= ~$fixedbitmask;
        $insn |= $fixedbits;

        if (defined $constraint) {
            # user-specified constraint: evaluate in an environment
            # with variables set corresponding to the variable fields.
            my $v = eval_with_fields($insnname, $insn, $rec, "constraints", $constraint);
            if (!$v) {
                $constraintfailures++;
                if ($constraintfailures > 10000) {
                    print "10000 consecutive constraint failures for $insnname constraints string:\n$constraint\n";
                    exit (1);
                }
                next INSN;
            }
        }

        # OK, we got a good one
        $constraintfailures = 0;

        my $basereg;

        if (defined $memblock) {
            # This is a load or store. We simply evaluate the block,
            # which is expected to be a call to a function which emits
            # the code to set up the base register and returns the
            # number of the base register.
            # Default alignment requirement for ARM is 4 bytes,
            # we use 16 for Aarch64, although often unnecessary and overkill.
            align(16);
            $basereg = eval_with_fields($insnname, $insn, $rec, "memory", $memblock);
        }

        insn32($insn);

        if (defined $memblock) {
            # Clean up following a memory access instruction:
            # we need to turn the (possibly written-back) basereg
            # into an offset from the base of the memory block,
            # to avoid making register values depend on memory layout.
            # $basereg -1 means the basereg was a target of a load
            # (and so it doesn't contain a memory address after the op)
            if ($basereg != -1) {
                write_mov_ri($basereg, 0);
            }
        }
        return;
    }
}

sub write_risuop($)
{
    # instr with bits (28:27) == 0 0 are UNALLOCATED
    my ($op) = @_;
    insn32(0x00005af0 | $op);
}

sub write_test_code($)
{
    my ($params) = @_;

    my $condprob = $params->{ 'condprob' };
    my $numinsns = $params->{ 'numinsns' };
    my $fp_enabled = $params->{ 'fp_enabled' };
    my $outfile = $params->{ 'outfile' };

    my %insn_details = %{ $params->{ 'details' } };
    my @keys = @{ $params->{ 'keys' } };

    if ($params->{ 'bigendian' } eq 1) {
        set_endian(1);
    }

    open_bin($outfile);

    # convert from probability that insn will be conditional to
    # probability of forcing insn to unconditional
    $condprob = 1 - $condprob;

    # TODO better random number generator?
    srand(0);

    print "Generating code using patterns: @keys...\n";
    progress_start(78, $numinsns);

    write_risuop($OP_SETUPBEGIN);

    if (grep { defined($insn_details{$_}->{blocks}->{"memory"}) } @keys) {
        write_memblock_setup();
    }

    # memblock setup doesn't clean its registers, so this must come afterwards.
    write_random_register_data($fp_enabled);

    write_risuop($OP_SETUPEND);

    write_risuop($OP_COMPARE);

    for my $i (1..$numinsns) {
        my $insn_enc = $keys[int rand (@keys)];
        #dump_insn_details($insn_enc, $insn_details{$insn_enc});
        my $forcecond = (rand() < $condprob) ? 1 : 0;
        gen_one_insn($forcecond, $insn_details{$insn_enc});
        write_risuop($OP_COMPARE);
        # Rewrite the registers periodically. This avoids the tendency
        # for the VFP registers to decay to NaNs and zeroes.
        if ($periodic_reg_random && ($i % 100) == 0) {
            write_risuop($OP_SETUPBEGIN);
            write_random_register_data($fp_enabled);
            write_risuop($OP_SETUPEND);
            write_risuop($OP_COMPARE);
        }
        progress_update($i);
    }
    write_risuop($OP_TESTEND);
    progress_end();
    close_bin();
}

1;
