#!/usr/bin/perl -w
###############################################################################
# Copyright (c) 2010 Linaro Limited
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the Eclipse Public License v1.0
# which accompanies this distribution, and is available at
# http://www.eclipse.org/legal/epl-v10.html
#
# Contributors:
#     Peter Maydell (Linaro) - initial implementation
#     Claudio Fontana (Linaro) - initial aarch64 support
#     Jose Ricardo Ziviani (IBM) - modularize risugen
###############################################################################

# risugen -- generate a test binary file for use with risu
# See 'risugen --help' for usage information.
package risugen_arm;

use strict;
use warnings;

use risugen_common;

require Exporter;

our @ISA    = qw(Exporter);
our @EXPORT = qw(write_test_code);

my $periodic_reg_random = 1;
my $enable_aarch64_ld1 = 0;

# Note that we always start in ARM mode even if the C code was compiled for
# thumb because we are called by branch to a lsbit-clear pointer.
# is_thumb tracks the mode we're actually currently in (ie should we emit
# an arm or thumb insn?); test_thumb tells us which mode we need to switch
# to to emit the insns under test.
# Use .mode aarch64 to start in Aarch64 mode.

my $is_aarch64 = 0; # are we in aarch64 mode?
# For aarch64 it only makes sense to put the mode directive at the
# beginning, and there is no switching away from aarch64 to arm/thumb.

my $is_thumb = 0;   # are we currently in Thumb mode?
my $test_thumb = 0; # should test code be Thumb mode?

# Maximum alignment restriction permitted for a memory op.
my $MAXALIGN = 64;

# An instruction pattern as parsed from the config file turns into
# a record like this:
#   name          # name of the pattern
#   width         # 16 or 32
#   fixedbits     # values of the fixed bits
#   fixedbitmask  # 1s indicate locations of the fixed bits
#   blocks        # hash of blockname->contents (for constraints etc)
#   fields        # array of arrays, each element is [ varname, bitpos, bitmask ]
#
# We store these in the insn_details hash.

# Valid block names (keys in blocks hash)
my %valid_blockname = ( constraints => 1, memory => 1 );

# for thumb only
sub thumb_align4()
{
    if ($bytecount & 3) {
        insn16(0xbf00);  # NOP
    }
}

# used for aarch64 only for now
sub data_barrier()
{
    if ($is_aarch64) {
        insn32(0xd5033f9f); # DSB SYS
    }
}

# The space 0xE7F___F_ is guaranteed to always UNDEF
# and not to be allocated for insns in future architecture
# revisions. So we use it for our 'do comparison' and
# 'end of test' instructions.
# We fill in the middle bit with a randomly selected
# 'e5a' just in case the space is being used by somebody
# else too.

# For Thumb the equivalent space is 0xDExx
# and we use 0xDEEx.

sub write_thumb_risuop($)
{
    my ($op) = @_;
    insn16(0xdee0 | $op);
}

sub write_arm_risuop($)
{
    my ($op) = @_;
    insn32(0xe7fe5af0 | $op);
}

sub write_aarch64_risuop($)
{
    # instr with bits (28:27) == 0 0 are UNALLOCATED
    my ($op) = @_;
    insn32(0x00005af0 | $op);
}

sub write_risuop($)
{
    my ($op) = @_;
    if ($is_thumb) {
        write_thumb_risuop($op);
    } elsif ($is_aarch64) {
        write_aarch64_risuop($op);
    } else {
        write_arm_risuop($op);
    }
}

sub write_switch_to_thumb()
{
    # Switch to thumb if we're not already there
    if (!$is_thumb) {
        # Note that we have to clean up r0 afterwards
        # so it isn't tainted with a value which depends
        # on PC (and which might differ between hw and
        # qemu/valgrind/etc)
        insn32(0xe28f0001);     # add r0, pc, #1
        insn32(0xe12fff10);     # bx r0
        insn16(0x4040);         # eor r0,r0 (enc T1)
        $is_thumb = 1;
    }
}

sub write_switch_to_arm()
{
    # Switch to ARM mode if we are in thumb mode
    if ($is_thumb) {
        thumb_align4();
        insn16(0x4778);  # bx pc
        insn16(0xbf00);  # nop
        $is_thumb = 0;
    }
}

sub write_switch_to_test_mode()
{
    # Switch to whichever mode we need for test code
    if ($is_aarch64) {
        return; # nothing to do
    }

    if ($test_thumb) {
        write_switch_to_thumb();
    } else {
        write_switch_to_arm();
    }
}

# sign extend a 32bit reg into a 64bit reg
sub write_sxt32($$)
{
    my ($rd, $rn) = @_;
    die "write_sxt32: invalid operation for this arch.\n" if (!$is_aarch64);

    insn32(0x93407c00 | $rn << 5 | $rd);
}

sub write_add_rri($$$)
{
    my ($rd, $rn, $i) = @_;
    my $sh;

    die "write_add_rri: invalid operation for this arch.\n" if (!$is_aarch64);

    if ($i >= 0 && $i < 0x1000) {
        $sh = 0;
    } elsif (($i & 0xfff) || $i >= 0x1000000) {
        die "invalid immediate for this arch,\n";
    } else {
        $sh = 1;
        $i >>= 12;
    }
    insn32(0x91000000 | ($rd << 0) | ($rn << 5) | ($i << 10) | ($sh << 22));
}

sub write_sub_rrr($$$)
{
    my ($rd, $rn, $rm) = @_;

    if ($is_aarch64) {
        insn32(0xcb000000 | ($rm << 16) | ($rn << 5) | $rd);

    } elsif ($is_thumb) {
        # enc T2
        insn16(0xeba0 | $rn);
        insn16(0x0000 | ($rd << 8) | $rm);
    } else {
        # enc A1
        insn32(0xe0400000 | ($rn << 16) | ($rd << 12) | $rm);
    }
}

# valid shift types
my $SHIFT_LSL = 0;
my $SHIFT_LSR = 1;
my $SHIFT_ASR = 2;
my $SHIFT_ROR = 3;

sub write_sub_rrrs($$$$$)
{
    # sub rd, rn, rm, shifted
    my ($rd, $rn, $rm, $type, $imm) = @_;
    $type = $SHIFT_LSL if $imm == 0;
    my $bits = $is_aarch64 ? 64 : 32;

    if ($imm == $bits && ($type == $SHIFT_LSR || $type == $SHIFT_ASR)) {
        $imm = 0;
    }
    die "write_sub_rrrs: bad shift immediate $imm\n" if $imm < 0 || $imm > ($bits - 1);

    if ($is_aarch64) {
        insn32(0xcb000000 | ($type << 22) | ($rm << 16) | ($imm << 10) | ($rn << 5) | $rd);

    } elsif ($is_thumb) {
        # enc T2
        my ($imm3, $imm2) = ($imm >> 2, $imm & 3);
        insn16(0xeba0 | $rn);
        insn16(($imm3 << 12) | ($rd << 8) | ($imm2 << 6) | ($type << 4) | $rm);
    } else {
        # enc A1
        insn32(0xe0400000 | ($rn << 16) | ($rd << 12) | ($imm << 7) | ($type << 5) | $rm);
    }
}

sub write_mov_rr($$)
{
    my ($rd, $rm) = @_;

    if ($is_aarch64) {
        # using ADD 0x11000000 */
        insn32(0x91000000 | ($rm << 5) | $rd);

    } elsif ($is_thumb) {
        # enc T3
        insn16(0xea4f);
        insn16(($rd << 8) | $rm);
    } else {
        # enc A1
        insn32(0xe1a00000 | ($rd << 12) | $rm);
    }
}

sub write_mov_ri16($$$)
{
    # Write 16 bits of immediate to register.
    my ($rd, $imm, $is_movt) = @_;

    die "write_mov_ri16: invalid operation for this arch.\n" if ($is_aarch64);
    die "write_mov_ri16: immediate $imm out of range\n" if (($imm & 0xffff0000) != 0);

    if ($is_thumb) {
        # enc T3
        my ($imm4, $i, $imm3, $imm8) = (($imm & 0xf000) >> 12,
                                        ($imm & 0x0800) >> 11,
                                        ($imm & 0x0700) >> 8,
                                        ($imm & 0x00ff));
        insn16(0xf240 | ($is_movt << 7) | ($i << 10) | $imm4);
        insn16(($imm3 << 12) | ($rd << 8) | $imm8);
    } else {
        # enc A2
        my ($imm4, $imm12) = (($imm & 0xf000) >> 12,
                              ($imm & 0x0fff));
        insn32(0xe3000000 | ($is_movt << 22) | ($imm4 << 16) | ($rd << 12) | $imm12);
    }
}

sub write_mov_ri($$)
{
    my ($rd, $imm) = @_;
    my $highhalf = ($imm >> 16) & 0xffff;

    if ($is_aarch64) {
        if ($imm < 0) {
            # MOVN
            insn32(0x92800000 | ((~$imm & 0xffff) << 5) | $rd);
            # MOVK, LSL 16
            insn32(0xf2a00000 | ($highhalf << 5) | $rd) if $highhalf != 0xffff;
        } else {
            # MOVZ
            insn32(0x52800000 | (($imm & 0xffff) << 5) | $rd);
            # MOVK, LSL 16
            insn32(0xf2a00000 | ($highhalf << 5) | $rd) if $highhalf != 0;
        }
    } else {
        write_mov_ri16($rd, ($imm & 0xffff), 0);
        write_mov_ri16($rd, $highhalf, 1) if $highhalf;
    }
}

sub write_addpl_rri($$$)
{
    my ($rd, $rn, $imm) = @_;
    die "write_addpl: invalid operation for this arch.\n" if (!$is_aarch64);

    insn32(0x04605000 | ($rn << 16) | (($imm & 0x3f) << 5) | $rd);
}

sub write_addvl_rri($$$)
{
    my ($rd, $rn, $imm) = @_;
    die "write_addvl: invalid operation for this arch.\n" if (!$is_aarch64);

    insn32(0x04205000 | ($rn << 16) | (($imm & 0x3f) << 5) | $rd);
}

sub write_rdvl_ri($$)
{
    my ($rd, $imm) = @_;
    die "write_rdvl: invalid operation for this arch.\n" if (!$is_aarch64);

    insn32(0x04bf5000 | (($imm & 0x3f) << 5) | $rd);
}

sub write_madd_rrrr($$$$)
{
    my ($rd, $rn, $rm, $ra) = @_;
    die "write_madd: invalid operation for this arch.\n" if (!$is_aarch64);

    insn32(0x9b000000 | ($rm << 16) | ($ra << 10) | ($rn << 5) | $rd);
}

sub write_msub_rrrr($$$$)
{
    my ($rd, $rn, $rm, $ra) = @_;
    die "write_msub: invalid operation for this arch.\n" if (!$is_aarch64);

    insn32(0x9b008000 | ($rm << 16) | ($ra << 10) | ($rn << 5) | $rd);
}

sub write_mul_rrr($$$)
{
    my ($rd, $rn, $rm) = @_;
    write_madd_rrrr($rd, $rn, $rm, 31);
}

# write random fp value of passed precision (1=single, 2=double, 4=quad)
sub write_random_fpreg_var($)
{
    my ($precision) = @_;
    my $randomize_low = 0;

    if ($precision != 1 && $precision != 2 && $precision != 4) {
        die "write_random_fpreg: invalid precision.\n";
    }

    my ($low, $high);
    my $r = rand(100);
    if ($r < 5) {
        # +-0 (5%)
        $low = $high = 0;
        $high |= 0x80000000 if (rand() < 0.5);
    } elsif ($r < 10) {
        # NaN (5%)
        # (plus a tiny chance of generating +-Inf)
        $randomize_low = 1;
        $high = irand(0xffffffff) | 0x7ff00000;
    } elsif ($r < 15) {
        # Infinity (5%)
        $low = 0;
        $high = 0x7ff00000;
        $high |= 0x80000000 if (rand() < 0.5);
    } elsif ($r < 30) {
        # Denormalized number (15%)
        # (plus tiny chance of +-0)
        $randomize_low = 1;
        $high = irand(0xffffffff) & ~0x7ff00000;
    } else {
        # Normalized number (70%)
        # (plus a small chance of the other cases)
        $randomize_low = 1;
        $high = irand(0xffffffff);
    }

    for (my $i = 1; $i < $precision; $i++) {
        if ($randomize_low) {
            $low = irand(0xffffffff);
        }
        insn32($low);
    }
    insn32($high);
}

sub write_random_double_fpreg()
{
    my ($low, $high);
    my $r = rand(100);
    if ($r < 5) {
        # +-0 (5%)
        $low = $high = 0;
        $high |= 0x80000000 if (rand() < 0.5);
    } elsif ($r < 10) {
        # NaN (5%)
        # (plus a tiny chance of generating +-Inf)
        $low = irand(0xffffffff);
        $high = irand(0xffffffff) | 0x7ff00000;
    } elsif ($r < 15) {
        # Infinity (5%)
        $low = 0;
        $high = 0x7ff00000;
        $high |= 0x80000000 if (rand() < 0.5);
    } elsif ($r < 30) {
        # Denormalized number (15%)
        # (plus tiny chance of +-0)
        $low = irand(0xffffffff);
        $high = irand(0xffffffff) & ~0x7ff00000;
    } else {
        # Normalized number (70%)
        # (plus a small chance of the other cases)
        $low = irand(0xffffffff);
        $high = irand(0xffffffff);
    }
    insn32($low);
    insn32($high);
}

sub write_random_single_fpreg()
{
    my ($value);
    my $r = rand(100);
    if ($r < 5) {
        # +-0 (5%)
        $value = 0;
        $value |= 0x80000000 if (rand() < 0.5);
    } elsif ($r < 10) {
        # NaN (5%)
        # (plus a tiny chance of generating +-Inf)
        $value = irand(0xffffffff) | 0x7f800000;
    } elsif ($r < 15) {
        # Infinity (5%)
        $value = 0x7f800000;
        $value |= 0x80000000 if (rand() < 0.5);
    } elsif ($r < 30) {
        # Denormalized number (15%)
        # (plus tiny chance of +-0)
        $value = irand(0xffffffff) & ~0x7f800000;
    } else {
        # Normalized number (70%)
        # (plus a small chance of the other cases)
        $value = irand(0xffffffff);
    }
    insn32($value);
}

sub write_random_arm_fpreg()
{
    # Write out 64 bits of random data intended to
    # initialise an FP register.
    # We tweak the "randomness" here to increase the
    # chances of picking interesting values like
    # NaN, -0.0, and so on, which would be unlikely
    # to occur if we simply picked 64 random bits.
    if (rand() < 0.5) {
        write_random_fpreg_var(2); # double
    } else {
        write_random_fpreg_var(1); # single
        write_random_fpreg_var(1); # single
    }
}

sub write_random_arm_regdata($)
{
    my ($fp_enabled) = @_;
    # TODO hardcoded, also no d16-d31 initialisation
    my $vfp = $fp_enabled ? 2 : 0; # 0 : no vfp, 1 : vfpd16, 2 : vfpd32
    write_switch_to_arm();
    
    # initialise all registers
    if ($vfp == 1) {
        insn32(0xe28f0008);    # add r0, pc, #8
        insn32(0xecb00b20);    # vldmia r0!, {d0-d15}
    } elsif ($vfp == 2) {
        insn32(0xe28f000c);    # add r0, pc, #12
        insn32(0xecb00b20);    # vldmia r0!, {d0-d15}
        insn32(0xecf00b20);    # vldmia r0!, {d16-d31}
    } else {
        insn32(0xe28f0004);    # add r0, pc, #4
    }
    
    insn32(0xe8905fff);        # ldmia r0, {r0-r12,r14}
    my $datalen = 14;
    $datalen += (32 * $vfp);
    insn32(0xea000000 + ($datalen-1));    # b next
    for (0..(($vfp * 16) - 1)) { # NB: never done for $vfp == 0
        write_random_arm_fpreg();
    }
    #  .word [14 words of data for r0..r12,r14]
    for (0..13) {
        insn32(irand(0xffffffff));
    }
    # next:
    # clear the flags (NZCVQ and GE): msr APSR_nzcvqg, #0
    insn32(0xe32cf000);
}

sub write_random_aarch64_fpdata()
{
    # load floating point / SIMD registers
    my $align = 16;
    my $datalen = 32 * 16 + $align;
    write_pc_adr(0, (3 * 4) + ($align - 1)); # insn 1
    write_align_reg(0, $align);              # insn 2
    write_jump_fwd($datalen);                # insn 3

    # align safety
    for (my $i = 0; $i < ($align / 4); $i++) {
        insn32(irand(0xffffffff));
    };

    for (my $rt = 0; $rt <= 31; $rt++) {
        write_random_fpreg_var(4); # quad
    }

    if ($enable_aarch64_ld1) {
        # enable only when we have ld1
        for (my $rt = 0; $rt <= 31; $rt += 4) {
            insn32(0x4cdf2c00 | $rt); # ld1 {v0.2d-v3.2d}, [x0], #64
        }
    } else {
        # temporarily use LDP instead
        for (my $rt = 0; $rt <= 31; $rt += 2) {
            insn32(0xacc10000 | ($rt + 1) << 10 | ($rt)); # ldp q0,q1,[x0],#32
        }
    }
}

sub write_random_aarch64_svedata()
{
    # Load SVE registers
    my $align = 16;
    my $vq = 16;                             # quadwords per vector
    my $veclen = 32 * $vq * 16;
    my $predlen = 16 * $vq * 2;
    my $datalen = $veclen + $predlen;

    write_pc_adr(0, 2 * 4);     # insn 1
    write_jump_fwd($datalen);   # insn 2

    for (my $rt = 0; $rt <= 31; $rt++) {
        for (my $q = 0; $q < $vq; $q++) {
            write_random_fpreg_var(4); # quad
        }
    }
    for (my $rt = 0; $rt <= 15; $rt++) {
        for (my $q = 0; $q < $vq; $q++) {
            insn16(irand(0xffff));
        }
    }

    for (my $rt = 0; $rt <= 31; $rt++) {
        # ldr z$rt, [x0, #$rt, mul vl]
        insn32(0x85804000 + $rt + (($rt & 7) << 10) + (($rt & 0x18) << 13));
    }

    write_add_rri(0, 0, $veclen);

    for (my $rt = 0; $rt <= 15; $rt++) {
        # ldr p$rt, [x0, #$pt, mul vl]
        insn32(0x85800000 + $rt + (($rt & 7) << 10) + (($rt & 0x18) << 13));
    }
}

sub write_random_aarch64_regdata($$)
{
    my ($fp_enabled, $sve_enabled) = @_;
    # clear flags
    insn32(0xd51b421f);     # msr nzcv, xzr

    # Load floating point / SIMD registers
    # (one or the other as they overlap)
    if ($sve_enabled) {
        write_random_aarch64_svedata();
    } elsif ($fp_enabled) {
        write_random_aarch64_fpdata();
    }

    # general purpose registers
    for (my $i = 0; $i <= 30; $i++) {
        # TODO full 64 bit pattern instead of 32
        write_mov_ri($i, irand(0xffffffff));
    }
}

sub write_random_register_data($$)
{
    my ($fp_enabled, $sve_enabled) = @_;

    if ($is_aarch64) {
        write_random_aarch64_regdata($fp_enabled, $sve_enabled);
    } else {
        write_random_arm_regdata($fp_enabled);
    }

    write_risuop($OP_COMPARE);
}

# put PC + offset into a register.
# this must emit an instruction of 4 bytes.
sub write_pc_adr($$)
{
    my ($rd, $imm) = @_;

    if ($is_aarch64) {
        # C2.3.5 PC-relative address calculation
        # The ADR instruction adds a signed, 21-bit value of the pc that fetched this instruction,
        my ($immhi, $immlo) = ($imm >> 2, $imm & 0x3);
        insn32(0x10000000 | $immlo << 29 | $immhi << 5 | $rd);
    } else {
        # A.2.3 ARM Core Registers:
        # When executing an ARM instruction, PC reads as the address of the current insn plus 8.
        $imm -= 8;
        insn32(0xe28f0000 | $rd << 12 | $imm);

    }
}

# clear bits in register to satisfy alignment.
# Must use exactly 4 instruction-bytes (one instruction on arm)
sub write_align_reg($$)
{
    my ($rd, $align) = @_;
    die "bad alignment!" if ($align < 2);

    if ($is_aarch64) {
        # and rd, rd, ~(align - 1)    ; A64 BIC imm is an alias for AND

        # Unfortunately we need to calculate the immr/imms/N values to get
        # our desired immediate value. In this case we want to use an element
        # size of 64, which means that N is 1, immS is the length of run of
        # set bits in the mask, and immR is the rotation.
        # N = 1, immR = 64 - ctz, imms = 63 - ctz
        # (Note that an all bits-set mask is not encodable here, but
        # the requirement for $align to be at least 2 avoids that.)
        my $cnt = ctz($align);
        insn32(0x92000000 | 1 << 22 | (64 - $cnt) << 16 | (63 - $cnt) << 10 | $rd << 5 | $rd);
    } else {
        # bic rd, rd, (align - 1)
        insn32(0xe3c00000 | $rd << 16 | $rd << 12 | ($align - 1));
    }
}

# jump ahead of n bytes starting from next instruction
sub write_jump_fwd($)
{
    my ($len) = @_;

    if ($is_aarch64) {
        # b pc + len
        insn32(0x14000000 | (($len / 4) + 1));
    } else {
        # b pc + len
        insn32(0xea000000 | (($len / 4) - 1));
    }
}

sub write_memblock_setup()
{
    # Write code which sets up the memory block for loads and stores.
    # We set r0 to point to a block of 8K length
    # of random data, aligned to the maximum desired alignment.
    write_switch_to_arm();

    my $align = $MAXALIGN;
    my $datalen = 8192 + $align;
    if (($align > 255) || !is_pow_of_2($align) || $align < 4) {
        die "bad alignment!";
    }

    # set r0 to (datablock + (align-1)) & ~(align-1)
    # datablock is at PC + (4 * 4 instructions) = PC + 16
    write_pc_adr(0, (4 * 4) + ($align - 1)); # insn 1
    write_align_reg(0, $align);              # insn 2
    write_risuop($OP_SETMEMBLOCK);           # insn 3
    write_jump_fwd($datalen);                # insn 4

    for (my $i = 0; $i < $datalen / 4; $i++) {
        insn32(irand(0xffffffff));
    }
    # next:

}

sub write_set_fpscr_arm($)
{
    my ($fpscr) = @_;
    write_switch_to_arm();
    # movw r0, imm16
    insn32(0xe3000000 | ($fpscr & 0xfff) | (($fpscr & 0xf000) << 4));
    # movt r0, imm16
    insn32(0xe3400000 | (($fpscr & 0xf0000000) >> 12) | (($fpscr & 0x0fff0000) >> 16));
    # vmsr fpscr, r0
    insn32(0xeee10a10);
}

sub write_set_fpscr_aarch64($)
{
    # on aarch64 we have split fpcr and fpsr registers.
    # Status will be initialized to 0, while user param controls fpcr.
    my ($fpcr) = @_;
    write_mov_ri(0, 0);
    insn32(0xd51b4420); #  msr fpsr, x0
    write_mov_ri(0, $fpcr);
    insn32(0xd51b4400); #  msr fpcr, x0
}

sub write_set_fpscr($)
{
    my ($fpscr) = @_;
    if ($is_aarch64) {
        write_set_fpscr_aarch64($fpscr);
    } else {
        write_set_fpscr_arm($fpscr);
    }
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

# XXX claudio: this seems to get the full address, not the offset.
sub write_get_offset()
{
    # Emit code to get a random offset within the memory block, of the
    # right alignment, into r0
    # We require the offset to not be within 256 bytes of either
    # end, to (more than) allow for the worst case data transfer, which is
    # 16 * 64 bit regs
    my $offset = (rand(2048 - 512) + 256) & ~($alignment_restriction - 1);
    write_mov_ri(0, $offset);
    write_risuop($OP_GETMEMBLOCK);
}

# Return the log2 of the memory size of an operation described by dtype.
sub dtype_msz($)
{
    my ($dtype) = @_;
    my $dth = $dtype >> 2;
    my $dtl = $dtype & 3;
    return $dtl >= $dth ? $dth : 3 - $dth;
}

sub reg($@)
{
    my ($base, @trashed) = @_;
    write_get_offset();
    # Now r0 is the address we want to do the access to,
    # so just move it into the basereg
    if ($base != 0) {
        write_mov_rr($base, 0);
        write_mov_ri(0, 0);
    }
    if (grep $_ == $base, @trashed) {
        return -1;
    }
    return $base;
}

sub reg_plus_imm($$@)
{
    # Handle reg + immediate addressing mode
    my ($base, $imm, @trashed) = @_;
    if ($imm == 0) {
        return reg($base, @trashed);
    }

    write_get_offset();
    # Now r0 is the address we want to do the access to,
    # so set the basereg by doing the inverse of the
    # addressing mode calculation, ie base = r0 - imm
    # We could do this more cleverly with a sub immediate.
    if ($base != 0) {
        write_mov_ri($base, $imm);
        write_sub_rrr($base, 0, $base);
        # Clear r0 to avoid register compare mismatches
        # when the memory block location differs between machines.
        write_mov_ri(0, 0);
    } else {
        # We borrow r1 as a temporary (not a problem
        # as long as we don't leave anything in a register
        # which depends on the location of the memory block)
        write_mov_ri(1, $imm);
        write_sub_rrr($base, 0, 1);
    }
    if (grep $_ == $base, @trashed) {
        return -1;
    }
    return $base;
}

sub reg_plus_imm_pl($$@)
{
    # Handle reg + immediate addressing mode
    my ($base, $imm, @trashed) = @_;
    if ($imm == 0) {
        return reg($base, @trashed);
    }
    write_get_offset();

    # Now r0 is the address we want to do the access to,
    # so set the basereg by doing the inverse of the
    # addressing mode calculation, ie base = r0 - imm
    #
    # Note that addpl has a 6-bit immediate, but ldr has a 9-bit
    # immediate, so we need to be able to support larger immediates.

    if (-$imm >= -32 && -$imm <= 31) {
        write_addpl_rri($base, 0, -$imm);
    } else {
        # We borrow r1 and r2 as a temporaries (not a problem
        # as long as we don't leave anything in a register
        # which depends on the location of the memory block)
        write_mov_ri(1, 0);
        write_mov_ri(2, $imm);
        write_addpl_rri(1, 1, 1);
        write_msub_rrrr($base, 1, 2, 0);
    }
    if (grep $_ == $base, @trashed) {
        return -1;
    }
    return $base;
}

sub reg_plus_imm_vl($$@)
{
    # The usual address formulation is
    #   elements = VL DIV esize
    #   mbytes = msize DIV 8
    #   addr = base + imm * elements * mbytes
    # Here we compute
    #   scale = log2(esize / msize)
    #   base + (imm * VL) >> scale
    my ($base, $imm, $scale, @trashed) = @_;
    if ($imm == 0) {
        return reg($base, @trashed);
    }
    write_get_offset();

    # Now r0 is the address we want to do the access to,
    # so set the basereg by doing the inverse of the
    # addressing mode calculation, ie base = r0 - imm
    #
    # Note that rdvl/addvl have a 6-bit immediate, but ldr has a 9-bit
    # immediate, so we need to be able to support larger immediates.

    use integer;
    my $mul = 1 << $scale;
    my $imm_div = $imm / $mul;

    if ($imm == $imm_div * $mul && -$imm_div >= -32 && -$imm_div <= 31) {
        write_addvl_rri($base, 0, -$imm_div);
    } elsif ($imm >= -32 && $imm <= 31) {
        write_rdvl_ri(1, $imm);
        write_sub_rrrs($base, 0, 1, $SHIFT_ASR, $scale);
    } else {
        write_rdvl_ri(1, 1);
        write_mov_ri(2, $imm);
        if ($scale == 0) {
            write_msub_rrrr($base, 1, 2, 0);
        } else {
            write_mul_rrr(1, 1, 2);
            write_sub_rrrs($base, 0, 1, $SHIFT_ASR, $scale);
        }
    }
    if (grep $_ == $base, @trashed) {
        return -1;
    }
    return $base;
}

sub reg_minus_imm($$@)
{
    my ($base, $imm, @trashed) = @_;
    return reg_plus_imm($base, -$imm, @trashed);
}

sub reg_plus_reg_shifted($$$@)
{
    # handle reg + reg LSL imm addressing mode
    my ($base, $idx, $shift, @trashed) = @_;
    if ($shift < 0 || $shift > 4 || (!$is_aarch64 && $shift == 4)) {

        print ("\n(shift) $shift\n");
        print ("\n(arch) $is_aarch64\n");
        die "reg_plus_reg_shifted: bad shift size\n";
    }
    my $savedidx = 0;
    if ($idx == 0) {
        # save the index into some other register for the
        # moment, because the risuop will trash r0
        $idx = 1;
        $idx++ if $idx == $base;
        $savedidx = 1;
        write_mov_rr($idx, 0);
    }

    # Get a random offset within the memory block, of the
    # right alignment.
    write_get_offset();
    # Now r0 is the address we want to do the access to,
    # so set the basereg by doing the inverse of the
    # addressing mode calculation, ie base = r0 - idx LSL imm
    # LSL x is shift type 0, 
    write_sub_rrrs($base, 0, $idx, $SHIFT_LSL, $shift);
    if ($savedidx) {
        # We can move this back to r0 now
        write_mov_rr(0, $idx);
    } elsif ($base != 0) {
        write_mov_ri(0, 0);
    }
    if (grep $_ == $base, @trashed) {
        return -1;
    }
    return $base;
}

sub reg_plus_reg($$@)
{
    my ($base, $idx, @trashed) = @_;
    return reg_plus_reg_shifted($base, $idx, 0, @trashed);
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
        for my $tuple (@{ $rec->{fields} }) {
            my ($var, $pos, $mask) = @$tuple;
            my $val = ($insn >> $pos) & $mask;
            # XXX (claudio) ARM-specific - maybe move to arm.risu?
            # Check constraints here:
            # not allowed to use or modify sp or pc
            if (!$is_aarch64) {
                next INSN if ($var =~ /^r/ && (($val == 13) || ($val == 15)));
                # Some very arm-specific code to force the condition field
                # to 'always' if requested.
                if ($forcecond) {
                    if ($var eq "cond") {
                        $insn &= ~ ($mask << $pos);
                        $insn |= (0xe << $pos);
                    }
                }
            }
        }
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
            # Default alignment requirement for ARM is 4 bytes,
            # we use 16 for Aarch64, although often unnecessary and overkill.
            if ($is_aarch64) {
                align(16);
            } else {
                align(4);
            }
            $basereg = eval_with_fields($insnname, \$insn, $rec, "memory", $memblock);

            if ($is_aarch64) {
                data_barrier();
            }
        }

        if ($is_thumb) {
            # Since the encoding diagrams in the ARM ARM give 32 bit
            # Thumb instructions as low half | high half, we
            # flip the halves here so that the input format in
            # the config file can be in the same order as the ARM.
            # For a 16 bit Thumb instruction the generated insn is in
            # the high halfword (because we didn't bother to readjust
            # all the bit positions in parse_config_file() when we
            # got to the end and found we only had 16 bits).
            insn16($insn >> 16);
            if ($insnwidth == 32) {
                insn16($insn & 0xffff);
            }
        } else {
            # ARM is simple, always a 32 bit word
            insn32($insn);
        }

        if (defined $memblock) {
            # Clean up following a memory access instruction:
            # we need to turn the (possibly written-back) basereg
            # into an offset from the base of the memory block,
            # to avoid making register values depend on memory layout.
            # $basereg -1 means the basereg was a target of a load
            # (and so it doesn't contain a memory address after the op)

            if ($is_aarch64) {
                data_barrier();
            }

            if ($basereg != -1) {
                write_mov_ri(0, 0);
                write_risuop($OP_GETMEMBLOCK);
                write_sub_rrr($basereg, $basereg, 0);
                write_mov_ri(0, 0);
            }
            write_risuop($OP_COMPAREMEM);
        }
        return;
    }
}

sub write_test_code($$$$$$$$)
{
    my ($params) = @_;

    my $arch = $params->{ 'arch' };
    my $subarch = $params->{ 'subarch' };

    if ($subarch && $subarch eq 'aarch64') {
        $test_thumb = 0;
        $is_aarch64 = 1;
    } elsif ($subarch && $subarch eq 'thumb') {
        $test_thumb = 1;
        $is_aarch64 = 0;
    } else {
        $test_thumb = 0;
        $is_aarch64 = 0;
    }

    my $condprob = $params->{ 'condprob' };
    my $fpscr = $params->{ 'fpscr' };
    my $numinsns = $params->{ 'numinsns' };
    my $fp_enabled = $params->{ 'fp_enabled' };
    my $sve_enabled = $params->{ 'sve_enabled' };
    my $outfile = $params->{ 'outfile' };

    my %insn_details = %{ $params->{ 'details' } };
    my @keys = @{ $params->{ 'keys' } };

    open_bin($outfile);

    # convert from probability that insn will be conditional to
    # probability of forcing insn to unconditional
    $condprob = 1 - $condprob;

    # TODO better random number generator?
    srand($params->{ 'srand' });

    print "Generating code using patterns: @keys...\n";
    progress_start(78, $numinsns);

    if ($fp_enabled) {
        write_set_fpscr($fpscr);
    }

    if (grep { defined($insn_details{$_}->{blocks}->{"memory"}) } @keys) {
        write_memblock_setup();
    }
    # memblock setup doesn't clean its registers, so this must come afterwards.
    write_random_register_data($fp_enabled, $sve_enabled);
    write_switch_to_test_mode();

    for my $i (1..$numinsns) {
        my $insn_enc = $keys[int rand (@keys)];
        #dump_insn_details($insn_enc, $insn_details{$insn_enc});
        my $forcecond = (rand() < $condprob) ? 1 : 0;
        gen_one_insn($forcecond, $insn_details{$insn_enc});
        write_risuop($OP_COMPARE);
        # Rewrite the registers periodically. This avoids the tendency
        # for the VFP registers to decay to NaNs and zeroes.
        if ($periodic_reg_random && ($i % 100) == 0) {
            write_random_register_data($fp_enabled, $sve_enabled);
            write_switch_to_test_mode();
        }
        progress_update($i);
    }
    write_risuop($OP_TESTEND);
    progress_end();
    close_bin();
}

1;
