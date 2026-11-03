package BodyLines;

sub new {
    my ($class, @lines) = @_;

    return bless { lines => [@lines] }, $class;
}

sub getline {
    my ($self) = @_;

    return shift @{ $self->{lines} };
}

sub close {
    $main::closed++;
}

package main;

our $closed = 0;

my $app = sub {
    my ($environ) = @_;

    # The record separator as the application sees it at the start of the
    # request: a string in hex, "ref" for a reference, or "undef".
    my $rs = !defined $/ ? 'undef' : ref $/ ? 'ref' : unpack('H*', $/);

    # Empty lines are not the end of the body.  undef is.
    my @lines = $environ->{'QUERY_STRING'} eq 'empty' ? ('', 'ab', '', 'cd')
                                                       : ('ab', 'cd');

    return ['200', ['X-RS' => $rs, 'X-Closed' => $closed],
            BodyLines->new(@lines)];
};
