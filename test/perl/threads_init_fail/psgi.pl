# Each interpreter of the application loads this script.  %ENV writes go
# to the process environment, and a new interpreter copies it, so the
# second load sees the mark of the first load and fails.
die "second load" if $ENV{NXT_TEST_PERL_LOADED};

$ENV{NXT_TEST_PERL_LOADED} = 1;

my $app = sub {
    return ['200', [], ['ok']];
};
