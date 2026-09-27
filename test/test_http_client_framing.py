import pytest

from unit.http import HTTP1

# HTTP1._response_complete() tells recvall(framed=True) when a keep-alive
# response is complete.  These cases pin its framing rules.  They need no
# application.

HEAD = b'HTTP/1.1 200 OK\r\n'


@pytest.mark.parametrize(
    'data, complete',
    [
        # Content-Length.
        (HEAD + b'Content-Length: 5\r\n\r\nhello', True),
        (HEAD + b'Content-Length: 5\r\n\r\nhel', False),
        (HEAD + b'Content-Length: 0\r\n\r\n', True),
        (HEAD + b'content-length:  3 \r\n\r\nabc', True),
        (HEAD + b'Content-Length: x\r\n\r\nabc', False),
        # The headers are not complete yet.
        (HEAD + b'Content-Length: 0\r\n', False),
        # Chunked.
        (
            HEAD
            + b'Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n',
            True,
        ),
        (HEAD + b'Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n', False),
        (HEAD + b'Transfer-Encoding: chunked\r\n\r\n5\r\nhel', False),
        (HEAD + b'Transfer-Encoding: chunked\r\n\r\n0\r\n', False),
        (HEAD + b'Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n', True),
        # A chunk extension is not part of the size.
        (
            HEAD + b'Transfer-Encoding: chunked\r\n\r\n'
            b'5;name=value\r\nhello\r\n0\r\n\r\n',
            True,
        ),
        (HEAD + b'Transfer-Encoding: chunked\r\n\r\nzz\r\n', False),
        # No framing: the read ends on the timeout.
        (HEAD + b'Connection: close\r\n\r\nbody', False),
    ],
)
def test_http_client_response_complete(data, complete):
    assert HTTP1._response_complete(data) is complete
