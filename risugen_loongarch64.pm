#!/usr/bin/perl -w
###############################################################################
# Copyright (c) 2022 Loongson Technology Corporation Limited
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the Eclipse Public License v1.0
# which accompanies this distribution, and is available at
# http://www.eclipse.org/legal/epl-v10.html
#
# Contributors:
#     based on Peter Maydell (Linaro) - initial implementation
###############################################################################

# risugen -- generate a test binary file for use with risu
# See 'risugen --help' for usage information.
package risugen_loongarch64;

use strict;
use warnings;

use risugen_common;

require Exporter;

our @ISA    = qw(Exporter);
our @EXPORT = qw(write_test_code);

my $periodic_reg_random = 1;

# Maximum alignment restriction permitted for a memory op.
my $MAXALIGN = 64;

my $OP_COMPARE = 0;        # compare registers
my $OP_TESTEND = 1;        # end of test, stop
my $OP_SETMEMBLOCK = 2;    # r4 is address of memory block (8192 bytes)
my $OP_GETMEMBLOCK = 3;    # add the address of memory block to r4
my $OP_COMPAREMEM = 4;     # compare memory block

sub write_risuop($)
{
    my ($op) = @_;
    insn32(0x000001f0 | $op);
}

sub write_set_fcsr($)
{
    my ($fcsr) = @_;
    # movgr2fcsr r0, r0
    insn32(0x0114c000);
}

# Global used to communicate between align(x) and reg() etc.
my $alignment_restriction;

sub set_reg_w($)
{
    my($reg)=@_;
    # Set reg [0x0, 0x7FFFFFFF]

    # $reg << 33
    # slli.d  $reg, $reg, 33
    insn32(0x410000 | 33 << 10 | $reg << 5 | $reg);
    # $reg >> 33
    # srli.d  $reg, $reg, 33
    insn32(0x450000 | 33 << 10 | $reg << 5 | $reg);

    return $reg;
}

sub nanbox_s($)
{
    my ($fpreg)=@_;

    # Set $fpreg register high 32bit ffffffff
    # use r1 as a temp register
    # r1 = r1 | ~(r0)
    write_mov_ri(1, -1);

    # movgr2frh.w   $fpreg ,$1
    insn32(0x114ac00 | 1 << 5 | $fpreg);

    return $fpreg;
}

sub clean_lsx_result($)
{
    my ($vreg) = @_;

    # xvinsgr2vr.d vd, r0, 2;
    insn32(0x76ebe000 | 2 << 10 | $vreg);
    # xvinsgr2vr.d vd, r0, 3;
    insn32(0x76ebe000 | 3 << 10 | $vreg);

    return $vreg;
}

sub align($)
{
    my ($a) = @_;
    if (!is_pow_of_2($a) || ($a < 0) || ($a > $MAXALIGN)) {
        die "bad align() value $a\n";
    }
    $alignment_restriction = $a;
}

sub write_sub_rrr($$$)
{
    my ($rd, $rj, $rk) = @_;
    # sub.d rd, rj, rk
    insn32(0x00118000 | $rk << 10 | $rj << 5 | $rd);
}

sub write_mov_rr($$$)
{
    my($rd, $rj, $rk) = @_;
    # add.d rd, rj, r0
    insn32(0x00108000 | 0 << 10 | $rj << 5 | $rd);
}

sub write_mov_ri($$)
{
    my ($rd, $imm) = @_;

    if ($imm >= -0x80000000 && $imm <= 0x7fffffff) {
        # lu12i.w rd, si20
        insn32(0x14000000 | (($imm >> 12) & 0xfffff) << 5 | $rd);
        # ori rd, rd, ui12
        insn32(0x03800000 | ($imm & 0xfff) << 10 | $rd << 5 | $rd);
    } else {
        die "unhandled immediate load";
    }
}

sub write_get_offset()
{
    # Emit code to get a random offset within the memory block, of the
    # right alignment, into r4
    # We require the offset to not be within 256 bytes of either
    # end, to (more than) allow for the worst case data transfer, which is
    # 16 * 64 bit regs
    my $offset = (rand(2048 - 512) + 256) & ~($alignment_restriction - 1);
    write_mov_ri(4, $offset);
    write_risuop($OP_GETMEMBLOCK);
}

sub reg_plus_reg($$@)
{
    my ($base, $idx, @trashed) = @_;
    my $savedidx = 0;
    if ($idx == 4) {
        # Save the index into some other register for the
        # moment, because the risuop will trash r4.
        $idx = 5;
        $idx++ if $idx == $base;
        $savedidx = 1;
        write_mov_rr($idx, 4, 0);
    }
    # Get a random offset within the memory block, of the
    # right alignment.
    write_get_offset();

    write_sub_rrr($base, 4, $idx);
    if ($base != 4) {
        if ($savedidx) {
            write_mov_rr(4, $idx, 0);
            write_mov_ri($idx, 0);
        } else {
            write_mov_ri(4, 0);
        }
    } else {
        if ($savedidx) {
            write_mov_ri($idx, 0);
        }
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

    write_get_offset();
    # Now r4 is the address we want to do the access to,
    # so set the basereg by doing the inverse of the
    # addressing mode calculation, ie base = r4 - imm
    # We could do this more cleverly with a sub immediate.
    if ($base != 4) {
        write_mov_ri($base, $imm);
        write_sub_rrr($base, 4, $base);
        # Clear r4 to avoid register compare mismatches
        # when the memory block location differs between machines.
         write_mov_ri(4, 0);
    }else {
        # We borrow r1 as a temporary (not a problem
        # as long as we don't leave anything in a register
        # which depends on the location of the memory block)
        write_mov_ri(1, $imm);
        write_sub_rrr($base, 4, 1);
    }

    if (grep $_ == $base, @trashed) {
        return -1;
    }
    return $base;
}

sub write_pc_adr($$)
{
    my($rd, $imm) = @_;
    # pcaddi (si20 | 2bit 0) + pc
    insn32(0x18000000 | $imm << 5 | $rd);
}

sub write_and($$$)
{
    my($rd, $rj, $rk)  = @_;
    # and rd, rj, rk
    insn32(0x148000 | $rk << 10 | $rj << 5 | $rd);
}

sub write_align_reg($$)
{
    my ($rd, $align) = @_;
    # rd = rd & ~($align -1);
    # use r1 as a temp register.
    write_mov_ri(1, $align -1);
    write_sub_rrr(1, 0, 1);
    write_and($rd, $rd, 1);
}

sub write_jump_fwd($)
{
    my($len) = @_;
    # b pc + len
    my ($offslo, $offshi) = (($len / 4 + 1) & 0xffff, ($len / 4 + 1) >> 16);
    insn32(0x50000000 | $offslo << 10 | $offshi);
}

sub write_memblock_setup()
{
    my $align = $MAXALIGN;
    my $datalen = 8192 + $align;
    if (($align > 255) || !is_pow_of_2($align) || $align < 4) {
        die "bad alignment!";
    }

    # Set r4 to (datablock + (align-1)) & ~(align-1)
    # datablock is at PC + (4 * 4 instructions) = PC + 16
    write_pc_adr(4, (4 * 4) + ($align - 1)); #insn 1
    write_align_reg(4, $align);              #insn 2
    write_risuop($OP_SETMEMBLOCK);           #insn 3
    write_jump_fwd($datalen);                #insn 4

    for(my $i = 0; $i < $datalen / 4; $i++) {
        insn32(rand(0xffffffff));
    }
}

# Write random fp value of passed precision (1=single, 2=double, 4=quad)
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
        $high = rand(0xffffffff) | 0x7ff00000;
    } elsif ($r < 15) {
        # Infinity (5%)
        $low = 0;
        $high = 0x7ff00000;
        $high |= 0x80000000 if (rand() < 0.5);
    } elsif ($r < 30) {
        # Denormalized number (15%)
        # (plus tiny chance of +-0)
        $randomize_low = 1;
        $high = rand(0xffffffff) & ~0x7ff00000;
    } else {
        # Normalized number (70%)
        # (plus a small chance of the other cases)
        $randomize_low = 1;
        $high = rand(0xffffffff);
    }

    for (my $i = 1; $i < $precision; $i++) {
        if ($randomize_low) {
            $low = rand(0xffffffff);
        }
        insn32($low);
    }
    insn32($high);
}

sub write_random_loongarch64_fpdata()
{
    # Load floating point registers
    my $align = 16;
    my $datalen = 32 * 16 + $align;
    my $off = 0;
    write_pc_adr(5, (4 * 4) + $align);       # insn 1  pcaddi
    write_pc_adr(4, (3 * 4) + ($align - 1)); # insn 2  pcaddi
    write_align_reg(4, $align);              # insn 3  andi
    write_jump_fwd($datalen);                # insn 4  b pc + len

    # Align safety
    for (my $i = 0; $i < ($align / 4); $i++) {
        insn32(rand(0xffffffff));
    }

    for (my $i = 0; $i < 32; $i++) {
        write_random_fpreg_var(4); # double
    }

    $off = 0;
    for (my $i = 0; $i < 32; $i++) {
        my $tmp_reg = 6;
        # r5 is fp register initial val
        # r4 is aligned base address
        # copy memory from r5 to r4
        # ld.d r6, r5, $off
        # st.d r6, r4, $off
        # $off = $off + 16
        insn32(0x28c00000 | $off << 10 | 5 << 5 | $tmp_reg);
        insn32(0x29c00000 | $off << 10 | 4 << 5 | $tmp_reg);
        $off = $off + 8;
        insn32(0x28c00000 | $off << 10 | 5 << 5 | $tmp_reg);
        insn32(0x29c00000 | $off << 10 | 4 << 5 | $tmp_reg);
        $off = $off + 8;
    }

    $off = 0;
    for (my $i = 0; $i < 32; $i++) {
        # fld.d fd, r4, $off
        insn32(0x2b800000 | $off << 10 | 4 << 5 | $i);
        $off = $off + 16;
    }
}

sub write_random_regdata()
{
    # General purpose registers, skip r2
    write_mov_ri(1, rand(0x7fffffff)); # init r1
    for  (my $i = 3; $i < 32; $i++) {
        write_mov_ri($i, rand(0x7fffffff));
    }
}

sub write_random_register_data($)
{
    my ($fp_enabled) = @_;

    # Set fcc0 ~ fcc7
    # movgr2cf $fcc0, $zero
    insn32(0x114d800);
    # movgr2cf $fcc1, $zero
    insn32(0x114d801);
    # movgr2cf $fcc2, $zero
    insn32(0x114d802);
    # movgr2cf $fcc3, $zero
    insn32(0x114d803);
    # movgr2cf $fcc4, $zero
    insn32(0x114d804);
    # movgr2cf $fcc5, $zero
    insn32(0x114d805);
    # movgr2cf $fcc6, $zero
    insn32(0x114d806);
    # movgr2cf $fcc7, $zero
    insn32(0x114d807);

    if ($fp_enabled) {
        # Load floating point registers
        write_random_loongarch64_fpdata();

        # Write random LASX data.
        for (my $i = 0; $i < 32; $i++) {
            my $tmp_reg = 6;
            # $fi is lasx register initial value.
            # movfr2gr.d r6 fi
            insn32(0x114b800 | $i << 5 | $tmp_reg);
            # xvreplgr2vr_d  $i r6
            insn32(0x769f0c00 | 6 << 5 | $i);
        }
    }

    write_random_regdata();
    write_risuop($OP_COMPARE);
}

sub gen_one_insn($$)
{
    # Given an instruction-details array, generate an instruction
    my $constraintfailures = 0;

    INSN: while(1) {
        my ($forcecond, $rec) = @_;
        my $insn = int(rand(0xffffffff));
        my $insnname = $rec->{name};
        my $insnwidth = $rec->{width};
        my $fixedbits = $rec->{fixedbits};
        my $fixedbitmask = $rec->{fixedbitmask};
        my $constraint = $rec->{blocks}{"constraints"};
        my $memblock = $rec->{blocks}{"memory"};
        my $post = $rec->{blocks}{"post"};

        $insn &= ~$fixedbitmask;
        $insn |= $fixedbits;

        if (defined $constraint) {
            # User-specified constraint: evaluate in an environment
            # with variables set corresponding to the variable fields.
            my $v = eval_with_fields($insnname, $insn, $rec, "constraints", $constraint);
            if(!$v) {
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

        if (defined $post) {
            # The hook for doing things after emitting the instruction.
            my $resultreg;
            $resultreg = eval_with_fields($insnname, $insn, $rec, "post", $post);
        }

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
            write_risuop($OP_COMPAREMEM);
        }
        return;
    }
}

sub write_test_code($)
{
    my ($params) = @_;

    my $condprob = $params->{ 'condprob' };
    my $fcsr = $params->{'fpscr'};
    my $numinsns = $params->{ 'numinsns' };
    my $fp_enabled = $params->{ 'fp_enabled' };
    my $outfile = $params->{ 'outfile' };

    my %insn_details = %{ $params->{ 'details' } };
    my @keys = @{ $params->{ 'keys' } };

    open_bin($outfile);

    # Convert from probability that insn will be conditional to
    # probability of forcing insn to unconditional
    $condprob = 1 - $condprob;

    # TODO better random number generator?
    srand(0);

    print "Generating code using patterns: @keys...\n";
    progress_start(78, $numinsns);

    if ($fp_enabled) {
        write_set_fcsr($fcsr);
    }

    if (grep { defined($insn_details{$_}->{blocks}->{"memory"}) } @keys) {
        write_memblock_setup();
    }
    # Memblock setup doesn't clean its registers, so this must come afterwards.
    write_random_register_data($fp_enabled);

    for my $i (1..$numinsns) {
        my $insn_enc = $keys[int rand (@keys)];
        my $forcecond = (rand() < $condprob) ? 1 : 0;
        gen_one_insn($forcecond, $insn_details{$insn_enc});
        write_risuop($OP_COMPARE);
        # Rewrite the registers periodically. This avoids the tendency
        # for the VFP registers to decay to NaNs and zeroes.
        if ($periodic_reg_random && ($i % 100) == 0) {
            write_random_register_data($fp_enabled);
        }
        progress_update($i);
    }
    write_risuop($OP_TESTEND);
    progress_end();
    close_bin();
}

1;
