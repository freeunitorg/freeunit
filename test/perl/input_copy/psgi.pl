my $saved_in;

END {
    if (defined $saved_in) {
        $saved_in->read(my $buf, 4);
    }
}

my $app = sub {
    my ($environ) = @_;

    $saved_in = $environ->{'psgi.input'};

    open(my $fh, ">&", $environ->{'psgi.input'});

    my $len = int($environ->{'CONTENT_LENGTH'});
    $fh->read(my $body, $len);

    return ['200', ['Content-Length' => $len], [$body]];
};
