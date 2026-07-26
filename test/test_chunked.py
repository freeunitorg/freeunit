import re
import socket
import time

import pytest
from unit.applications.lang.python import ApplicationPython

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    client.load('mirror')

    assert 'success' in client.conf(
        {"http": {"chunked_transform": True}}, 'settings'
    )


def test_chunked():
    def chunks(chunks=[]):
        body = ''

        for c in chunks:
            body = f'{body}{len(c):x}\r\n{c}\r\n'

        resp = client.get(
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'Transfer-Encoding': 'chunked',
            },
            body=f'{body}0\r\n\r\n',
        )

        expect_body = ''.join(chunks)

        assert resp['status'] == 200
        assert resp['headers']['Content-Length'] == str(len(expect_body))
        assert resp['body'] == expect_body

    chunks()
    chunks(['1'])
    chunks(['0123456789'])
    chunks(['0123456789' * 128])
    chunks(['0123456789' * 512])
    chunks(['0123456789' * 128, '1', '1', '0123456789' * 128, '1'])


def test_chunked_pipeline():
    sock = client.get(
        no_recv=True,
        headers={
            'Host': 'localhost',
            'Transfer-Encoding': 'chunked',
        },
        body='1\r\n$\r\n0\r\n\r\n',
    )

    resp = client.get(
        sock=sock,
        headers={
            'Host': 'localhost',
            'Transfer-Encoding': 'chunked',
            'Connection': 'close',
        },
        body='1\r\n%\r\n0\r\n\r\n',
        raw_resp=True,
    )

    assert len(re.findall('200 OK', resp)) == 2
    assert len(re.findall('Content-Length: 1', resp)) == 2
    assert len(re.findall('$', resp)) == 1
    assert len(re.findall('%', resp)) == 1


def test_chunked_max_body_size():
    assert 'success' in client.conf(
        {'max_body_size': 1024, 'chunked_transform': True}, 'settings/http'
    )

    body = f'{2048:x}\r\n{"x" * 2048}\r\n0\r\n\r\n'

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'Transfer-Encoding': 'chunked',
            },
            body=body,
        )['status']
        == 413
    )


def test_chunked_after_last():
    resp = client.get(
        headers={
            'Host': 'localhost',
            'Connection': 'close',
            'Transfer-Encoding': 'chunked',
        },
        body='1\r\na\r\n0\r\n\r\n1\r\nb\r\n0\r\n\r\n',
    )

    assert resp['status'] == 200
    assert resp['headers']['Content-Length'] == '1'
    assert resp['body'] == 'a'


def test_chunked_transform():
    assert 'success' in client.conf(
        {"http": {"chunked_transform": False}}, 'settings'
    )

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'Transfer-Encoding': 'chunked',
            },
            body='0\r\n\r\n',
        )['status']
        == 411
    )


def test_chunked_invalid():
    # invalid chunkes

    def check_body(body):
        assert (
            client.get(
                headers={
                    'Host': 'localhost',
                    'Connection': 'close',
                    'Transfer-Encoding': 'chunked',
                },
                body=body,
            )['status']
            == 400
        )

    check_body('1\r\nblah\r\n0\r\n\r\n')
    check_body('1\r\n\r\n1\r\n0\r\n\r\n')
    check_body('1\r\n1\r\n\r\n0\r\n\r\n')

    # Non-hex chunk size.
    check_body('z\r\nX\r\n0\r\n\r\n')

    # Chunk size that overflows the size accumulator must be rejected,
    # not silently wrapped (guards nxt_size_is_sufficient in the chunk
    # parser).  17 hex digits overshoot a 64-bit accumulator.
    check_body('1' + 'f' * 16 + '\r\nX\r\n0\r\n\r\n')

    # invalid transfer encoding header

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'Transfer-Encoding': ['chunked', 'chunked'],
            },
            body='0\r\n\r\n',
        )['status']
        == 400
    ), 'two Transfer-Encoding headers'

    assert (
        client.get(
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'Transfer-Encoding': 'chunked',
                'Content-Length': '5',
            },
            body='0\r\n\r\n',
        )['status']
        == 400
    ), 'Transfer-Encoding and Content-Length'

    assert (
        client.get(
            http_10=True,
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'Transfer-Encoding': 'chunked',
            },
            body='0\r\n\r\n',
        )['status']
        == 400
    ), 'Transfer-Encoding HTTP/1.0'


def test_chunked_split_reads():
    # Each write is delivered to the router as its own read, so it parses
    # buffers that carry only framing bytes -- a chunk-size line, a terminal
    # section -- and produce no body slice.  Such a buffer used to be handed
    # back to its completion handler, which frees it, while the connection kept
    # reading into it: a use-after-free that killed the router (visible as a
    # dropped connection here, and as a heap-use-after-free on the sanitize
    # leg).  freeunitorg/freeunit#148 review.
    req_head = (
        b'POST / HTTP/1.1\r\nHost: localhost\r\n'
        b'Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n'
    )

    def check(writes, expect_body, with_head=True):
        sock = socket.create_connection(('127.0.0.1', 8080))
        sock.settimeout(15)

        try:
            if with_head:
                sock.sendall(req_head)

            for w in writes:
                # A short pause keeps each write in its own segment; without it
                # the kernel may coalesce them into a single read and the
                # framing-only buffer never occurs.
                time.sleep(0.2)
                sock.sendall(w)

            resp = b''
            while True:
                part = sock.recv(4096)
                if not part:
                    break
                resp += part

        finally:
            sock.close()

        assert resp != b'', f'router dropped the connection: {writes}'

        head, _, body = resp.partition(b'\r\n\r\n')
        assert head.split(b'\r\n')[0].split()[1] == b'200', f'status: {writes}'
        assert expect_body in body, f'body: {writes}'

    # Chunk-size line alone, then data, then the terminal section.
    check([b'5\r\n', b'hello\r\n', b'0\r\n\r\n'], b'hello')

    # Several framing-only reads in a row, across two chunks.
    check(
        [b'4\r\n', b'abcd\r\n', b'3\r\n', b'xyz\r\n', b'0\r\n\r\n'],
        b'abcdxyz',
    )

    # Terminal section split from the body.
    check([b'5\r\nhello\r\n', b'0\r\n', b'\r\n'], b'hello')

    # The headers and the chunk-size line in one read, the data later.  Here
    # the framing-only buffer is the *header* buffer, which stays linked in
    # h1p->buffers with the parsed fields pointing into it, so recycling it
    # crashed at request close (double completion) rather than on the next
    # read.
    check(
        [req_head + b'5\r\n', b'hello\r\n', b'0\r\n\r\n'],
        b'hello',
        with_head=False,
    )

    # Control: an empty chunked body complete in the first read.  A complete
    # terminal section returns from inside the parse loop, so this shape never
    # reaches the recycle -- it is here to keep it that way if that early
    # return is ever refactored.
    check([req_head + b'0\r\n\r\n'], b'', with_head=False)
