my $prev_io;

my $app = sub {
    my ($environ) = @_;

    open my $io, '<file';
    undef $prev_io;
    $prev_io = $io;

    return ['200', ['Content-Length' => 5], $io];
};
