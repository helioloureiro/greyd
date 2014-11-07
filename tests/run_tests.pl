#!/usr/bin/perl -w
#
# Run all supplied test files through valgrind using the perl TAP
# test harness to process results.
#

use TAP::Harness;

my %args = (
    color    => 1,
    failures => 1,
    exec     => [
        'valgrind', '-q', '--trace-children=yes', '--track-origins=yes', '--leak-check=full',
        '--error-exitcode=1', '--tool=memcheck'
    ]
    );

my $harness = TAP::Harness->new(\%args);
$harness->runtests(@ARGV);
