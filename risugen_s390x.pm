#!/usr/bin/perl -w
###############################################################################
# Copyright 2023 Red Hat Inc.
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the Eclipse Public License v1.0
# which accompanies this distribution, and is available at
# http://www.eclipse.org/legal/epl-v10.html
#
# Contributors:
#     Thomas Huth - initial implementation (based on risugen_ppc64.pm etc.)
###############################################################################

# risugen -- generate a test binary file for use with risu
# See 'risugen --help' for usage information.
package risugen_s390x;

use strict;
use warnings;

use risugen_common;

require Exporter;

our @ISA    = qw(Exporter);
our @EXPORT = qw(write_test_code);

my $periodic_reg_random = 1;

# Maximum alignment restriction permitted for a memory op.
my $MAXALIGN = 64;

sub write_mov_ri($$$)
{
    my ($r, $imm_h, $imm_l) = @_;

    # LGFI
    insn16(0xc0 << 8 | $r << 4 | 0x1);
    insn32($imm_l);
    # IIHF r,imm_high
    insn16(0xc0 << 8 | $r << 4 | 0x8);
    insn32($imm_h);
}

sub write_mov_fp($$)
{
    my ($r, $imm) = @_;

    write_mov_ri(0, ~$imm, $imm);
    # LDGR
    insn32(0xb3c1 << 16 | $r << 4);
}

sub write_random_regdata()
{
    # Floating point registers
    for (my $i = 0; $i < 16; $i++) {
        write_mov_fp($i, irand(0xffffffff));
    }

    # Load FPC (via r0)
    write_mov_ri(0, 0, (irand(0xffffffff) & 0x00fcff77));
    insn32(0xb3840000);

    # general purpose registers
    for (my $i = 0; $i < 16; $i++) {
        write_mov_ri($i, irand(0xffffffff), irand(0xffffffff));
    }
}

sub write_random_register_data()
{
    write_random_regdata();
    write_risuop($OP_COMPARE);
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
            die "memblock handling has not been implemented yet."
        }

        if ($insnwidth == 16) {
            insn16(($insn >> 16) & 0xffff);
        } else {
            insn32($insn);
        }

        return;
    }
}

sub write_risuop($)
{
    my ($op) = @_;
    insn32(0x835a0f00 | $op);
}

sub write_test_code($)
{
    my ($params) = @_;

    my $condprob = $params->{ 'condprob' };
    my $numinsns = $params->{ 'numinsns' };
    my $outfile = $params->{ 'outfile' };

    my %insn_details = %{ $params->{ 'details' } };
    my @keys = @{ $params->{ 'keys' } };

    set_endian(1);

    open_bin($outfile);

    # convert from probability that insn will be conditional to
    # probability of forcing insn to unconditional
    $condprob = 1 - $condprob;

    # TODO better random number generator?
    srand($params->{ 'srand' });

    print "Generating code using patterns: @keys...\n";
    progress_start(78, $numinsns);

    if (grep { defined($insn_details{$_}->{blocks}->{"memory"}) } @keys) {
        write_memblock_setup();
    }

    # memblock setup doesn't clean its registers, so this must come afterwards.
    write_random_register_data();

    for my $i (1..$numinsns) {
        my $insn_enc = $keys[int rand (@keys)];
        #dump_insn_details($insn_enc, $insn_details{$insn_enc});
        my $forcecond = (rand() < $condprob) ? 1 : 0;
        gen_one_insn($forcecond, $insn_details{$insn_enc});
        write_risuop($OP_COMPARE);
        # Rewrite the registers periodically. This avoids the tendency
        # for the VFP registers to decay to NaNs and zeroes.
        if ($periodic_reg_random && ($i % 100) == 0) {
            write_random_register_data();
        }
        progress_update($i);
    }
    write_risuop($OP_TESTEND);
    progress_end();
    close_bin();
}

1;
