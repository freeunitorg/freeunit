my $prev_io;

my $app = sub {
    my ($environ) = @_;

    # 1 if the handle of the previous response is still open.
    my $open = (defined $prev_io && defined fileno($prev_io)) ? 1 : 0;

    open my $io, '<file';
    $prev_io = $io;

    return ['200', ['Content-Length' => 5, 'X-Prev-Open' => $open], $io];
};
