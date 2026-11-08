package InputReader;

sub new {
    my ($class, $input) = @_;

    return bless { input => $input }, $class;
}

sub DESTROY {
    my ($self) = @_;

    $main::read_len = $self->{input}->read(my $buf, 4);
}

package main;

our $read_len;

my $app = sub {
    my ($environ) = @_;

    my @headers = (
        'X-Pid'  => $$,
        'X-Read' => defined $read_len ? $read_len : 'none',
    );

    undef $read_len;

    if ($environ->{'QUERY_STRING'} eq 'streaming') {
        return sub {
            my ($responder) = @_;

            my $writer = $responder->(['200', \@headers]);
            $writer->close;

            $read_len = $environ->{'psgi.input'}->read(my $buf, 4);
        };
    }

    $environ->{'test.input_reader'} =
        InputReader->new($environ->{'psgi.input'});

    return ['200', \@headers, []];
};
