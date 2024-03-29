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

my %num;

my $unusableregs = 1<<(31-1);
#my $unusableregs = 1<<(31-1) | 1<<(31-13);

sub set_num($$)
{
    my ($name, $n) = @_;
    $num{$name} = $n;
}

sub get_num($)
{
    my ($name) = @_;
    return $num{$name};
}

sub regs_wrapped($$)
{
    my ($rt,$nb) = @_;
    my $nr = int (($nb + 3) / 4);
    my $regs = (-1 << (64-$nr)) >> $rt;
    my $regswrapped = ($regs >> 32) | ($regs & 0xffffffff);
    set_num("regs", $regswrapped);
    return $regswrapped;
}

sub usable(@)
{
    my $regs = 0;
    foreach my $reg (@_) {
        $regs |= 1<<(31-$reg);
    }
    return (($regs & $unusableregs) == 0);
}

sub usable_wrapped($)
{
    my ($regs) = @_;
    return (($regs & $unusableregs) == 0);
}

sub write_add_ri($$$)
{
    my ($rt, $ra, $imm) = @_;

    # addi rt, ra, immd
    insn32((0xe << 26) | ($rt << 21) | ($ra << 16) | ($imm & 0xffff));
}

sub write_subf_r($$$)
{
    my ($rt, $ra, $rb) = @_;

    # subf rt, ra, rb
    insn32((31 << 26) | ($rt << 21) | ($ra << 16) | ($rb << 11) | (40 << 1));
}

sub write_mov_ri($$)
{
    my ($rd, $imm) = @_;

    if ((($imm & 0xffff8000) != 0) && (($imm & 0xffff8000) != 0xffff8000)) {
        # lis rd,immediate@h
        insn32(0xf << 26 | $rd << 21 | (($imm >> 16) & 0xffff));
        if ($imm & 0xffff) {
            # ori rd,rd,immediate@l
            insn32((0x18 << 26) | ($rd << 21) | ($rd << 16) | ($imm & 0xffff));
        }
    }
    else {
        # li rd,immediate = addi rd, 0, $imm ; includes sign extension
        insn32(0xe << 26 | $rd << 21 | ($imm & 0xffff));
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
        write_store_64(irand(0xffffffff), irand(0xffffffff), 0x10); # any floating-point number
        #write_store_64(irand(0xfffff), irand(0xfffff), 0x10); # denormalized floating-point number
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
    # li r23, 0
    write_mov_ri(23, 0);

    # mtcrf 0xff,r23 ; zero the condition register
    insn32((31 << 26) | (23 << 21) | (255 << 12) | (144 << 1));

    # mtxer r23 ; zero the xer register
    insn32((31 << 26) | (23 << 21) | ((1 & 31) << 16) | ((1 >> 5) << 11) | (467 << 1));

    # li r23, random
    write_mov_ri(23, irand(0xffffffff));
    # mtctr r23 ; radomize the ctr register
    insn32((31 << 26) | (23 << 21) | ((9 & 31) << 16) | ((9 >> 5) << 11) | (467 << 1));

    # li r23, random
    write_mov_ri(23, irand(0xffffffff));
    # mtmq r23 ; radomize the mq register
    insn32((31 << 26) | (23 << 21) | ((0 & 31) << 16) | ((0 >> 5) << 11) | (467 << 1));

    # general purpose registers
    for (my $i = 0; $i < 32; $i++) {
        if (usable($i)) {
            write_mov_ri($i, irand(0xffffffff));
        }
    }
}

sub clear_vr_registers()
{
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

    # bl +4
    insn32((18 << 26) | (4) | (1));

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
    return $offset;
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

    if ($ra) {
        # addi $ra, r1, 0
        write_add_ri($ra, 1, 0);
        # li $rb, 32
        write_mov_ri($rb, 32);
        if (grep $_ == $ra, @trashed) {
            return -1;
        }
        return $ra;
    }
    else {
        # addi $rb, r1, 32
        write_add_ri($rb, 1, 32);
        if (grep $_ == $rb, @trashed) {
            return -1;
        }
        return $rb;
    }
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

sub branch_pre($$)
{
    my ($bd, $bdsize) = @_;

    if ($bdsize > 0) {
        # bit field displacement

        my $sign = $bd >> ($bdsize - 1);
        $bd = ($bd & ((1<<$bdsize) - 1)) - ($sign ? (1<<$bdsize) : 0);
    }

    if ($bdsize < 24) {
        # branch conditional

        #          setup ctr: li r5, xxx
        #                     mtctr r5

        #          setup ccr: li r5, xxx
        #                     mtcrf 0xff,r5

        #          calculate: bl +4
        #  0                  mflr r5
        #  4                  addi r5, r5, 20 | 16 + bd<<2
        #  8                  mtctr|lr r5

        if ($bdsize != -1) {
            # not branch conditional to ctr

            write_mov_ri(5, irand(3) - 1); # -1, 0, 1, 2
            # mtctr r5
            insn32((31 << 26) | (5 << 21) | ((9 & 31) << 16) | ((9 >> 5) << 11) | (467 << 1));
        }

        write_mov_ri(5, irand(0xffffffff));
        # mtcrf 0xff,r5
        insn32((31 << 26) | (5 << 21) | (255 << 12) | (144 << 1));

        if ($bdsize < 0) {
            # branch conditional to lr or ctr

            # bl +4
            insn32((18 << 26) | (4) | (1));
            # mflr r5
            insn32((31 << 26) | (5 << 21) | ((8 & 31) << 16) | ((8 >> 5) << 11) | (339 << 1));

            if ($bd < 0) {
                # addi r5, r5, 16
                write_add_ri(5, 5, 16);
            } else {
                # addi r5, r5, 12 + bd<<2
                write_add_ri(5, 5, 12 + ($bd << 2));
            }
            if ($bdsize == -1) {
                # mtctr r5
                insn32((31 << 26) | (5 << 21) | ((9 & 31) << 16) | ((9 >> 5) << 11) | (467 << 1));
            } else {
                # mtlr r5
                insn32((31 << 26) | (5 << 21) | ((8 & 31) << 16) | ((8 >> 5) << 11) | (467 << 1));
            }
        }
    }

    if ($bd < 0) {
        # negative displacement
        my $filler2 = irand(-$bd);

        #  12                 b +4+|disaplacement|
        #  16                 ... filler1 (0 or more)
        #                     b +8 + filler2<<2
        #                     ... filler2 (0 or more)
        #                     b -|disaplacement| # the instruction

        insn32((18 << 26) | ((1 - $bd - $filler2) << 2)); # branch to the negative offset branch
        for (my $i = $bd + 1 + $filler2; $i < 0; $i++) {
            write_add_ri(3,3,1); # filler before the negative offset branch
        }
        insn32((18 << 26) | (8 + ($filler2 << 2))); # jump over the negative offset branch
        for (my $i = $filler2; $i > 0; $i--) {
            write_add_ri(6,6,1); # filler before the negative offset branch
        }
    }
    else {
        # positive displacement

        #  12                 b +|disaplacement| # the instruction
        #  16                 ... filler (0 or more of these)
    }
}

sub branch_post($$)
{
    my ($bd, $bdsize) = @_;

    if ($bdsize > 0) {
        # bit field displacement

        my $sign = $bd >> ($bdsize - 1);
        $bd = ($bd & ((1<<$bdsize) - 1)) - ($sign ? (1<<$bdsize) : 0);
    }

    if ($bd <= 0) {
        $bd = 1;
    }
    for (my $i = $bd - 1; $i; $i--) {
        write_add_ri(7,7,1); # filler after the positive offset branch
    }
    for (my $i = irand(3); $i; $i--) {
        write_add_ri(4,4,1); # extra filler
    }

    if ($bdsize < 0) {
        if ($bdsize == -1) {
            # mfctr r8
            insn32((31 << 26) | (8 << 21) | ((9 & 31) << 16) | ((9 >> 5) << 11) | (339 << 1));
            write_subf_r(8,5,8);
            # mtctr r8
            insn32((31 << 26) | (8 << 21) | ((9 & 31) << 16) | ((9 >> 5) << 11) | (467 << 1));
        }
        else {
            # mflr r8
            insn32((31 << 26) | (8 << 21) | ((8 & 31) << 16) | ((8 >> 5) << 11) | (339 << 1));
            write_subf_r(8,5,8);
            # don't need to move to LR since LR compare is always PC relative.
        }
        write_mov_ri(5, irand(0x7fff));
    }
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
        my $post = $rec->{blocks}{"post"};
        my $pre = $rec->{blocks}{"pre"};

        $insn &= ~$fixedbitmask;
        $insn |= $fixedbits;

        if (defined $constraint) {
            # user-specified constraint: evaluate in an environment
            # with variables set corresponding to the variable fields.
            my $v = eval_with_fields($insnname, \$insn, $rec, "constraints", $constraint);
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

            # If you don't know that a basereg is going to be
            # trashed or not, then just have it cleared by setting
            # makezero to true.
            set_num("makezero", 0);

            # Default alignment requirement for ARM is 4 bytes,
            # we use 16 for Aarch64, although often unnecessary and overkill.
            align(16);
            $basereg = eval_with_fields($insnname, \$insn, $rec, "memory", $memblock);
        }

        if (defined $pre) {
            # The hook for doing things before the instruction.
            my $resultreg;
            $resultreg = eval_with_fields($insnname, \$insn, $rec, "pre", $pre);
        }

        insn32($insn);

        if (defined $post) {
            # The hook for doing things after emitting the instruction.
            my $resultreg;
            $resultreg = eval_with_fields($insnname, \$insn, $rec, "post", $post);
        }

        if (defined $memblock) {
            # Clean up following a memory access instruction:
            # we need to turn the (possibly written-back) basereg
            # into an offset from the base of the memory block,
            # to avoid making register values depend on memory layout.
            # $basereg -1 means the basereg was a target of a load
            # (and so it doesn't contain a memory address after the op)
            if ($basereg != -1) {
                if (get_num("makezero")) {
                    # This is set to true if we don't know that the
                    # base reg is still a memory related address.
                    write_mov_ri($basereg, 0);
                } else {
                    write_subf_r($basereg, 1, $basereg);
                }
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
    srand($params->{ 'srand' });

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
