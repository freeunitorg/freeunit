my $app = sub {
    my ($environ) = @_;

    my $name = 'X' x ($environ->{'HTTP_X_LENGTH'} // 300);

    return ['200', ['Content-Length' => 4, $name => 'value'], ['body']];
};
