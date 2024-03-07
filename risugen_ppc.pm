#!/usr/bin/perl -w
###############################################################################
# Copyright (c) 2017 John Arbuckle
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the Eclipse Public License v1.0
# which accompanies this distribution, and is available at
# http://www.eclipse.org/legal/epl-v10.html
#
# File: risugen_ppc.pm
# Date: 4-14-2017
# Description: Perl module for generating PowerPC instructions with risugen.
# Based on Jose Ricardo Ziviani (IBM) - ppc64 implementation
###############################################################################

# risugen -- generate a test binary file for use with risu
# See 'risugen --help' for usage information.
package risugen_ppc;

use strict;
use warnings;
use bigint;
no warnings "portable";

use risugen_common;

require Exporter;

our @ISA    = qw(Exporter);
our @EXPORT = qw(write_test_code);

my $periodic_reg_random = 1;

#
# Maximum alignment restriction permitted for a memory op.
my $MAXALIGN = 64;

# First available slot in the stack
my $available_stack_slot = 40;

# convert float value to IEEE 754
# input: floating point value (e.g. 1.23)
# output: IEEE 754 single precision value
sub convert_to_IEEE($)
{
    my $float_value = $_[0];
    my $ieee_value = unpack "L", pack "f", $float_value;
    return $ieee_value;
}

# convert float value to IEEE 754
# input: floating point value (e.g. 1.23)
# output: IEEE 754 double precision value
sub convert_to_IEEE64($)
{
    my $ieee_value = 0;
    my $float_value = $_[0];
    if ($float_value eq "qnan") {
        $ieee_value = 0x7ff8000000000000;
    } elsif ($float_value eq "snan") {
        $ieee_value = 0x7ff4000000000000;
    } elsif ($float_value eq "-FLT_MAX") {
        $ieee_value = 0xc7efffffe0000000;
    } elsif ($float_value eq "-FLT_MIN") {
        $ieee_value = 0xb810000000000000;
    } elsif ($float_value eq "-DBL_MAX") {
        $ieee_value = 0xffefffffffffffff;
    } elsif ($float_value eq "-DBL_MIN") {
        $ieee_value = 0x8010000000000000;
    } elsif ($float_value eq "FLT_MIN") {
        $ieee_value = 0x3810000000000000;
    } elsif ($float_value eq "FLT_MAX") {
        $ieee_value = 0x47efffffe0000000;
    } elsif ($float_value eq "DBL_MIN") {
        $ieee_value = 0x0010000000000000;
    } elsif ($float_value eq "DBL_MAX") {
        $ieee_value = 0x7fefffffffffffff;
    } else {
        $ieee_value = (unpack "Q", pack "d", $float_value);
    }
    return $ieee_value;
}

# returns a random double precision IEEE 754 value
# output: 64 bit number
sub get_random_IEEE_double_value()
{
    my @number_array = (0x40273333333333330, 0x405EDCCCCCCCCCCD,
                        0x40700A6666666666, 0x412E240CA45A1CAC,
                        0x407C3E8F5C28F5C3, 0x408DBF189374BC6A);
    my $size_of_array = scalar @number_array;
    my $index = int rand($size_of_array);
    return $number_array[$index];
}

# writes ppc assembly code to load a IEEE 754 double value
# input one: general purpose register number
# input two: immediate value (offset)
# output: IEEE 754 double value
sub load_double_float_imm($$)
{
    my ($rA, $number) = @_;

    my $ieee_double = get_random_IEEE_double_value(); # ieee 754 double float value

    # loading a 64 bit double value is going to take some work

    # load the higher 32 bits
    write_li32(10, $ieee_double >> 32);
    write_stw(10, $number);               # stw r10, $number(r1)

    # load the lower 32 bits
    write_li32(10, $ieee_double);
    write_stw(10, $number + 4);           # stw r10, $number+4 (r1)

    write_mr($rA, 1);                     # mr $rA, r1
    return $rA;
}

# writes ppc assembly code to load a IEEE 754 double value into memory
# input one: general purpose register number
# input two: general purpose register number
# output: IEEE 754 double value
sub load_double_float_indexed($$)
{
    my ($rA, $rB) = @_;

    my $ieee_double = get_random_IEEE_double_value(); # ieee 754 double float value

    # loading a 64 bit double value is going to take some work

    # load the higher 32 bits
    write_li32(10, $ieee_double >> 32);
    write_stw(10, $available_stack_slot);   # stw r10, $available_stack_slot(r1)

    # load the lower 32 bits
    write_li32(10, $ieee_double);
    write_stw(10, $available_stack_slot + 4);   # stw r10, ($available_stack_slot + 4)(r1)

    # setup the registers' value
    write_li32($rB, $available_stack_slot);
    write_mr($rA, 1);                           # mr $rA, r1
    return $rA;
}

# Setup a testing environment for an instruction that
# requires a double precision floating point value in a float register
# input: floating point register number
sub load_double_float_to_reg($)
{
    my ($fD) = @_;

    my $ieee_double = get_random_IEEE_double_value(); # ieee 754 double float value

    # loading a 64 bit double value is going to take some work

    # load the higher 32 bits
    write_li32(10, $ieee_double >> 32);
    write_stw(10, $available_stack_slot);   # stw r10, $available_stack_slot(r1)

    # load the lower 32 bits
    write_li32(10, $ieee_double);
    write_stw(10, $available_stack_slot + 4);   # stw r10, ($available_stack_slot + 4)(r1)

    write_lfd($fD, $available_stack_slot);      # lfd $fD, $available_stack_slot(r1)
    return -1;
}

# Setup a testing environment for an instruction that requires
# a single precision floating point value in a float register
# input: floating point register number
sub load_single_float_to_reg($)
{
    my ($fD) = @_;

    # load a random single precision floating point value into memory
    my $float_value = rand();
    my $ieee_value = convert_to_IEEE($float_value);

    write_li32(10, $ieee_value);
    write_stw(10, $available_stack_slot);   # stw r10, $available_stack_slot(r1)
    write_lfs($fD, $available_stack_slot);  # lfs $fD, $available_stack_slot(r1)

    return -1;  # this just says not not clean up any register
}

# write the addi instruction
# first input: destination register
# second input: source register
# third input: number
sub write_addi($$$)
{
    my ($rD, $rA, $number) = @_;

    # addi rD, rA, number
    insn32((0xe << 26) | ($rD << 21) | ($rA << 16) | ($number & 0xffff));
}

# writes the mr instruction
# first input: destination register
# second input: source register
sub write_mr($$)
{
    my ($rD, $rS) = @_;
    insn32(31 << 26 | $rS << 21 | $rD << 16 | $rS << 11 | 444 << 1 | 0);
}

# writes a stb instruction
# first input: register number
# second input: offset from stack pointer
sub write_stb($$)
{
    my ($rS, $offset) = @_;
    insn32(38 << 26 | $rS << 21 | 1 << 16 | $offset);
}

# writes a stw instruction
# first input: register number
# second input: offset from stack pointer
sub write_stw($$)
{
    my ($rS, $offset) = @_;
    insn32(36 << 26 | $rS << 21 | 1 << 16 | $offset);
}

# writes a lfs instruction
# first input: floating point register number
# second input: offset from stack pointer
sub write_lfs($$)
{
    my ($fD, $offset) = @_;
    insn32(48 << 26 | $fD << 21 | 1 << 16 | $offset);
}

# writes a lfd instruction
# first input: floating point register number
# second input: offset value
sub write_lfd($$)
{
    my ($fD, $offset) = @_;
    insn32(50 << 26 | $fD << 21 | 1 << 16 | $offset);
}

# writes li or lis with (optional ori) instructions
# first input: register number
# second input: value
sub write_li32($$)
{
    my ($register, $value) = @_;
    if ((($value & 0xffff8000) != 0) && (($value & 0xffff8000) != 0xffff8000)) {
        # lis
        insn32(15 << 26 | $register << 21 | 0 << 16 | (($value >> 16) & 0xffff));
        if ($value & 0xffff) {
            # ori
            insn32(24 << 26 | $register << 21 | $register << 16 | ($value & 0xffff));
        }
    } else {
        #li
        insn32(0xe << 26 | $register << 21 | ($value & 0xffff));
    }
}

# writes the mtfsb0 instruction
# input: fpscr field number
sub write_mtfsb0($)
{
    my $crbD = $_[0];
    insn32(63 << 26 | $crbD << 21 | 70 << 1 | 0);
}

# writes the mtfsb1 instruction
# input: fpscr field number
sub write_mtfsb1($)
{
    my $crbD = $_[0];
    insn32(63 << 26 | $crbD << 21 | 38 << 1 | 0);
}

sub write_mtfsfi($$)
{
    my ($crfD,$imm) = @_;
    insn32(63 << 26 | $crfD << 23 | $imm << 12 | 134 << 1 | 0);
}

sub write_mtfsf($$)
{
    my ($FM,$frB) = @_;
    insn32(63 << 26 | $FM << 17 | $frB << 11 | 711 << 1 | 0);
}

# writes the mtxer instruction
# first input: source register
sub write_mtxer($)
{
    my ($rS) = @_;
    insn32(31 << 26 | $rS << 21 | 16 << 12 | 467 << 1 | 0);
}

# writes the mtcrf instruction
# first input: condition register mask
# second input: source register
sub write_mtcrf($$)
{
    my ($CRM, $rS) = @_;
    insn32(31 << 26 | $rS << 21 | $CRM << 12 | 144 << 1);
}

# setup the testing environment for the lmw instruction
# first input: general purpose register number
# second input: general purpose regiser number
# third input: immediate value
# ouptut: general purpose register number
sub setup_lmw_test($$$)
{
    my ($rD, $rA, $imm) = @_;
    for(my $i = 0; $i < (32 - $rD); $i++) {
        write_li32(10, $i);
        write_stw(10, $imm + ($i * 4));     # stw r10, ($imm + $i * 4)(r1)
    }
    write_mr($rA, 1);                       # mr $rA, r1
    return $rA;
}

# setup the testing environment for the lswi instruction
# first input: general purpose register number
# second input: general purpose register number
# third input: number of bytes to load
# output: general purpose register number
sub setup_lswi_test($$$)
{
    my ($rD, $rA, $NB) = @_;
    my $imm;

    write_li32(10, 0xabcdef12);

    # fill the memory with $NB bytes of data
    for(my $i = 0; $i < $NB; $i++) {
        $imm = $available_stack_slot + $i * 4;
        write_stw(10, $imm);                        # stw r10, $imm(r1)
    }
    write_mr($rA, 1);                               # mr $rA, r1
    write_addi($rA, $rA, $available_stack_slot);    # addi $rA, $rA, $available_stack_slot
    return $rA;
}

# setup the testing environment for the lswx instruction
# first input: general purpose register number
# second input: general purpose register number
# third input: general purpose register number
# output: general purpose register number
sub setup_lswx_test($$$)
{
    my ($rD, $rA, $rB) = @_;
    my $bytes_per_register = 4;   # four bytes can fit in one general purpose register
    my $num_gpr = 32;   # the number of general purpose registers

    # This prevents LSWX from wrapping around and loading r0
    my $num_bytes = int rand(64);
    if (($num_bytes/$bytes_per_register) > ($num_gpr - $rD)) {
        $num_bytes = ($num_gpr - $rD) * $bytes_per_register;
    }

    # Prevent registers rA and rB from being in the loading path

    my $regA = 0;
    my $regB = 0;
    my $closest_register;
    my $overlap;

    # determine distance from register rD
    if ($rA - $rD > 0) {
        $regA = $rA;
    }

    if ($rB - $rD > 0) {
        $regB = $rB;
    }

    # both rA and rB are in the load path
    if ($regA > 0 && $regB > 0) {
        # find the register closest to rD
        $closest_register = ($regA < $regB) ? $regA : $regB;
    }

    # only rA is in the load path
    elsif ($regA != 0 && $regB == 0) {
        $closest_register = $regA;
    }

    # only rB is in the load path
    elsif ($regA == 0 && $regB != 0) {
        $closest_register = $regB;
    }

    # no register is in the load path
    else {
        $closest_register = -1;
    }

    # calculate new safe value for num_bytes
    if ($closest_register != -1 && $num_bytes >= ($closest_register - $rD) * $bytes_per_register) {
        $num_bytes = ($closest_register - $rD) * $bytes_per_register;
    }

    write_li32(10, $num_bytes);
    write_mtxer(10);                                     # mtxer r10

    # Fill memory with values
    for(my $i = 0; $i < $num_bytes; $i++) {
        write_li32(10, $i + 1);
        write_stb(10, $available_stack_slot + $i);       # stb r10, ($available_stack_slot + $i)(r1)
    }

    write_mr($rA, 1);                                    # mr $rA, r1
    write_li32($rB, $available_stack_slot);

    return $rA;
}

# writes the code that sets the general purpose registers
sub write_init_gpr_code()
{
    # Set the general purpose registers to a value.
    # Register r1 is the stack pointer.
    # It is left alone.
    for (my $i = 0; $i < 32; $i++) {
        if ($i == 1) {
            next;
        }
        write_li32($i, $i + 1);
    }
}

# set the initial value to all floating point registers
sub write_init_float_registers_code()
{
    my $value = 1.1;
    my $ieee_value;
    my $ieee_high_bits; # the upper 16 bits of $ieee_value
    my $ieee_low_bits;  # the lower 16 bits of $ieee_value

    for(my $i = 0; $i < 32; $i++) {
        $ieee_value = convert_to_IEEE($value + $i);
        write_li32(10, $ieee_value);
        write_stw(10, $available_stack_slot);   # stw r10, $available_stack_slot(r1)
        write_lfs($i, $available_stack_slot);   # lfs fD, $available_stack_slot(r1)
    }
}

# set the fpscr to the requested value
# input: value
sub write_init_fpscr_code($)
{
    my ($value) = @_;
    my $num_fpscr_fields = 32;

    for (my $i = 0; $i < $num_fpscr_fields; $i++) {
        if (($value >> $i) & 0x1) {
            write_mtfsb1($i);
        } else {
            write_mtfsb0($i);
        }
    }
}

# sets the xer register to zero
sub write_init_xer_code()
{
    my $rS = 10;
    my $value = 0;
    write_li32($rS, $value);
    write_mtxer($rS);
}

# sets the lr to pc
sub write_init_lr_code()
{
    # bl +4
    insn32((18 << 26) | (4) | (1));
}

# set the ctr to zero
sub write_init_ctr_code()
{
    my $rS = 10;
    my $value = 0;
    write_li32($rS, $value);
    insn32(0x7d4903a6);
}

# set the condition register to zero
sub write_init_cr_code()
{
    my $r10 = 10;
    my $value = 0;
    my $CRM = 0xff;
    write_li32($r10, $value);
    write_mtcrf($CRM, $r10);
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
    my $offset = (int rand(2048 - 512) + 256) & ~($alignment_restriction - 1);
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

my @one_values = (
    0x80000000, 0x80000001, 0x80000002,
    0xBFFFFFFD, 0xBFFFFFFE, 0xBFFFFFFF,
    0xC0000000, 0xC0000001, 0xC0000002,
    0xFFFF7FFD, 0xFFFF7FFE, 0xFFFF7FFF,
    0xffff8000, 0xffff8001, 0xffff8002,
    0xffffBFFD, 0xffffBFFE, 0xffffBFFF,
    0xffffC000, 0xffffC001, 0xffffC002,
    -3, -2, -1,
    0, 1,  2,
    0x00003ffd, 0x00003ffe, 0x00003fff,
    0x00004000, 0x00004001, 0x00004002,
    0x00007ffd, 0x00007ffe, 0x00007fff,
    0x00008000, 0x00008001, 0x00008002,
    0x3ffffffd, 0x3ffffffe, 0x3fffffff,
    0x40000000, 0x40000001, 0x40000002,
    0x7ffffffd, 0x7ffffffe, 0x7fffffff,
);
my $i_one_values = 0;

sub reg_from_values($@)
{
    my ($ra, @trashed) = @_;
    my $value;

    # clear or set carry
    my $carry = int ($i_one_values / scalar @one_values) % 2;
    write_li32($ra, $carry ? 0x20000000 : 0);
    write_mtxer($ra);

    # load operand rA
    $value = $one_values[$i_one_values % scalar @one_values];
    write_li32($ra, $value);

    $i_one_values++;

    return -1;
}

sub reg_from_two_values($$@)
{
    my ($ra, $rb, @trashed) = @_;
    my $value;

    # clear or set carry
    my $carry = int ($i_one_values / (scalar @one_values * scalar @one_values)) % 2;
    write_li32($ra, $carry ? 0x20000000 : 0);
    write_mtxer($ra);

    # load operand rA
    $value = $one_values[int (($i_one_values % (scalar @one_values * scalar @one_values)) / scalar @one_values) ];
    write_li32($ra, $value);

    # load operand rB
    $value = $one_values[     ($i_one_values % (scalar @one_values * scalar @one_values)) % scalar @one_values  ];
    write_li32($rb, $value);

    $i_one_values++;

    return -1;
}

my @imm_values = (
    0xffff8000, 0xffff8001, 0xffff8002,
    0xffffBFFD, 0xffffBFFE, 0xffffBFFF,
    0xffffC000, 0xffffC001, 0xffffC002,
    -3, -2, -1,
    0, 1,  2,
    0x00003ffd, 0x00003ffe, 0x00003fff,
    0x00004000, 0x00004001, 0x00004002,
    0x00007ffd, 0x00007ffe, 0x00007fff,
);

sub imm_test($$@) {
    my ($insn, $ra, @trashed) = @_;
    foreach my $carry (0,1) {
        foreach my $value (@one_values) {
            foreach my $imm (@imm_values) {
                my $low_bits;
                my $high_bits;

                # clear or set carry
                write_li32($ra, $carry ? 0x20000000 : 0);
                write_mtxer($ra);

                # load operand rA
                write_li32($ra, $value);

                insn32($insn | ($imm & 0xffff));
                write_risuop($OP_COMPARE);
            }
        }
    }
    return -1;
}

sub xer_update_test() {
    my $file = $ENV{testfile}; # ppctests.cpp
    open(CFILE, $file) or die "can't open $file: $!";
    while (<CFILE>) {
        if ( /^\s*xer_ov_test\("[^"]+", 0x(\w+)\);/ ) {
            write_li32(3, -1);
            write_mtxer(3);
            write_li32(3, 2);
            write_li32(4, 2);
            insn32(hex($1));
            write_risuop($OP_COMPARE);
        }
    }
    close(CFILE) or die "can't close $file: $!";
    return -1;
}

sub int_test() {
    my $file = $ENV{testfile}; # ppcinttests.csv
    open(CFILE, $file) or die "can't open $file: $!";
    while (<CFILE>) {
        write_li32(3, 0);
        write_mtcrf(0xff, 3);
        write_mtxer(3);
        if (/rA=0x(\w+)/) {
            write_li32(3, hex($1));
        }
        if (/rB=0x(\w+)/) {
            write_li32(4, hex($1));
        } else {
            write_li32(4, 0);
        }
        if (/,0x(\w+)/) {
            insn32(hex($1));
            write_risuop($OP_COMPARE);
        } else {
            die "can't close $file: $!";
        }
    }
    close(CFILE) or die "can't close $file: $!";
    return -1;
}

sub write_load_double($$) {
    my ($fD,$ieee_double) = @_;
    write_li32(3, $ieee_double >> 32);
    write_stw(3, $available_stack_slot);
    if (($ieee_double & 0xffffffff) != ($ieee_double >> 32)) {
        write_li32(3, $ieee_double);
    }
    write_stw(3, $available_stack_slot + 4);
    write_lfd($fD, $available_stack_slot);
}

sub float_test() {
    my $file = $ENV{testfile}; # ppcfloattests.csv
    open(CFILE, $file) or die "can't open $file: $!";

    write_li32(3, 0);
    write_stw(3, $available_stack_slot);  # stw r3, $available_stack_slot(r1)
    write_lfs(7, $available_stack_slot);  # lfs f6, $available_stack_slot(r1)

    while (<CFILE>) {
        write_load_double(3, 0);
        if (/frA=([^,]+)/) {
            write_load_double(4, convert_to_IEEE64($1));
            #printf("A:$1 ");
        } else {
            write_load_double(4, 0);
        }
        if (/frB=([^,]+)/) {
            write_load_double(5, convert_to_IEEE64($1));
            #printf("B:$1 ");
        } else {
            write_load_double(5, 0);
        }
        if (/frC=([^,]+)/) {
            write_load_double(6, convert_to_IEEE64($1));
            #printf("C:$1 ");
        } else {
            write_load_double(6, 0);
        }
        #printf("\n");

        write_li32(3, 0);
        write_li32(4, 0);
        write_mtcrf(0xff, 3);
        write_mtfsf(0xff, 7); # clear fpscr
        if (/round=(\w+)/) {
            my $round;
            if ($1 eq "RTN") {
                write_mtfsfi(7,0);
            } elsif ($1 eq "RTZ") {
                write_mtfsfi(7,1);
            } elsif ($1 eq "RPI") {
                write_mtfsfi(7,2);
            } elsif ($1 eq "RNI") {
                write_mtfsfi(7,3);
            } elsif ($1 eq "VEN") {
                write_mtfsb1(24);
            } else {
                die "unknown round mode $file: $!";
            }
        }

        if (/,0x(\w+)/) {
            insn32(hex($1));
        } else {
            die "can't close $file: $!";
        }
        write_risuop($OP_COMPARE);
    }
    close(CFILE) or die "can't close $file: $!";
    return -1;
}

sub reg_plus_reg($$@)
{
    my ($ra, $rb, @trashed) = @_;
    my $value;

    $value = irand(0xffffffff);

    # Add a value into the expected memory location
    write_li32(10, $value);
    write_stw(10, $available_stack_slot);   # stw r10, $available_stack_slot(r1)

    # setup the two registers to find the memory location
    write_mr($ra, 1);                       # mr $ra, r1
    write_li32($rb, $available_stack_slot);   # li $rb, $available_stack_slot
    return $ra;
}

sub reg_plus_imm($$@)
{
    my ($base, $imm, @trashed) = @_;

    # load a random floating point value into memory
    my $float_value = rand();
    my $ieee_value = convert_to_IEEE($float_value);

    write_li32(10, $ieee_value);
    write_stw(10, $imm);                  # stw r10, $imm(r1)
    write_mr($base, 1);                   # mr $base, r1
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
                if ($constraintfailures > 100000) {
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
                write_li32($basereg, 0);
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

    my $fpscr = $params->{ 'fpscr' };
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

    # initialize all the UISA registers
    write_init_gpr_code();

    if ($fp_enabled) {
        write_init_float_registers_code();
        write_init_fpscr_code($fpscr);
    }

    write_init_xer_code();
    write_init_lr_code();
    write_init_ctr_code();
    write_init_cr_code();
    write_risuop($OP_COMPARE);

    for my $i (1..$numinsns) {
        my $insn_enc = $keys[int rand (@keys)];
        #dump_insn_details($insn_enc, $insn_details{$insn_enc});
        my $forcecond = (rand() < $condprob) ? 1 : 0;
        gen_one_insn($forcecond, $insn_details{$insn_enc});
        write_risuop($OP_COMPARE);
        progress_update($i);
    }
    write_risuop($OP_TESTEND);
    progress_end();
    close_bin();
}

1;
