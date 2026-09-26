import gzip
import zlib
from pathlib import Path

import pytest

from unit.applications.proto import ApplicationProto

client = ApplicationProto()


@pytest.fixture(autouse=True)
def setup_method_fixture(temp_dir):
    assets_dir = f'{temp_dir}/assets'
    Path(assets_dir).mkdir(parents=True, exist_ok=True)
    Path(f'{assets_dir}/big.css').write_text(
        'body{color:red}' * 500, encoding='utf-8'
    )
    Path(f'{assets_dir}/tiny.css').write_text('ok', encoding='utf-8')
    # Above the 10-byte "min_length" below, so a 406 on this file can only
    # come from the media type, never from the length.
    Path(f'{assets_dir}/raw').write_text('raw' * 10, encoding='utf-8')

    assert 'success' in client.conf(
        {
            "settings": {
                "http": {
                    "compression": {
                        "types": ["text/css"],
                        "compressors": [
                            {"encoding": "gzip", "level": 5, "min_length": 10}
                        ],
                    }
                }
            },
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"share": f'{assets_dir}$uri'}}],
        }
    ), 'compression configure'


def test_static_compression_baseline():
    resp = client.get(
        url='/big.css',
        headers={
            'Host': 'localhost',
            'Accept-Encoding': 'gzip',
            'Connection': 'close',
        },
    )
    assert resp['status'] == 200, 'compressed 200'
    assert resp['headers']['Content-Encoding'] == 'gzip', 'gzip applied'


def test_static_compression_removed_between_requests():
    # #167: the compression state used to be process-global and allocated
    # from the router configuration that parsed it, so a configuration
    # without a "compression" block left it pointing into a freed pool and
    # the next request killed the router.  Pin the transition itself: every
    # other test in this file only ever configures compression, so the bug
    # reproduced through the file order of a whole suite run rather than
    # through any one file.
    headers = {
        'Host': 'localhost',
        'Accept-Encoding': 'gzip',
        'Connection': 'close',
    }

    resp = client.get(url='/big.css', headers=headers)
    assert resp['headers']['Content-Encoding'] == 'gzip', 'gzip before'

    assert 'success' in client.conf_delete(
        'settings/http/compression'
    ), 'compression removed'

    resp = client.get(url='/big.css', headers=headers)
    assert resp['status'] == 200, 'identity after the block is removed'
    assert 'Content-Encoding' not in resp['headers'], 'no coding after'

    # The second request is the one that used to find a freed pool: the
    # first may be answered before the old configuration is released.
    resp = client.get(url='/big.css', headers=headers)
    assert resp['status'] == 200, 'router still serving'


def test_static_compression_precondition_does_not_mask_406():
    # RFC 9110 Sect. 13.2.1: an ordinary failure outranks a precondition.  A
    # client that refuses every encoding cannot be served at all, so the
    # answer is 406 -- a validator must not turn that into 304 or 412.
    etag = client.get(url='/big.css')['headers']['ETag']

    def get(**headers):
        return client.get(
            url='/big.css',
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'Accept-Encoding': 'identity;q=0, *;q=0',
                **headers,
            },
        )

    assert get()['status'] == 406, 'unacceptable without a validator'
    assert get(**{'If-None-Match': '*'})['status'] == 406, '406 outranks 304'
    assert get(**{'If-None-Match': etag})['status'] == 406, '406 over 304'
    assert get(**{'If-Match': '"nope"'})['status'] == 406, '406 outranks 412'


def test_static_compression_304_carries_no_encoding():
    # A 304 sends no body, so it must not claim one is encoded, and the
    # compressor must not have been initialised for it either.
    etag = client.get(url='/big.css')['headers']['ETag']

    resp = client.get(
        url='/big.css',
        headers={
            'Host': 'localhost',
            'Connection': 'close',
            'Accept-Encoding': 'gzip',
            'If-None-Match': etag,
        },
    )

    assert resp['status'] == 304, 'not modified'
    assert resp['body'] == '', 'no body'
    assert 'Content-Encoding' not in resp['headers'], 'no Content-Encoding'
    assert 'Content-Length' not in resp['headers'], 'no Content-Length'


def test_static_compression_vary():
    # RFC 9110 Sect. 12.5.5: a response subject to proactive negotiation must
    # say what it varied on, or a shared cache may serve gzip bytes to a
    # client that cannot decode them.  This is the companion of the weak
    # entity-tag: that makes revalidation distinguish the codings, this makes
    # the cache key distinguish them.
    def get(**headers):
        return client.get(
            url='/big.css',
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    resp = get(**{'Accept-Encoding': 'gzip'})
    assert resp['status'] == 200
    assert resp['headers']['Content-Encoding'] == 'gzip', 'compressed'
    assert (
        resp['headers']['Vary'] == 'Accept-Encoding'
    ), 'Vary on the coded response'

    # The identity response is the one a cache must not reuse for a
    # gzip-capable client, so it needs the header just as much.
    resp = get()
    assert resp['status'] == 200
    assert 'Content-Encoding' not in resp['headers'], 'not compressed'
    assert (
        resp['headers']['Vary'] == 'Accept-Encoding'
    ), 'Vary on the identity response too'


@pytest.mark.parametrize(
    ('configured', 'expected'),
    [
        # No "Vary" of its own: the generated field stands unchanged.
        (None, 'Accept-Encoding'),
        # An empty value has no tokens to append to, so it is replaced
        # rather than appended to -- otherwise ", Accept-Encoding".
        ('', 'Accept-Encoding'),
        # A different header still needs the coding added: the response
        # varies on both.  An operator writing "Origin" is adding CORS and
        # is almost never aware Unit generates the field at all.
        ('Origin', 'Origin, Accept-Encoding'),
        ('a, b, c', 'a, b, c, Accept-Encoding'),
        # Already named: left exactly as written, and compared per token so
        # that "X-Accept-Encoding" does not count as a match.
        ('Accept-Encoding', 'Accept-Encoding'),
        ('a, Accept-Encoding', 'a, Accept-Encoding'),
        ('X-Accept-Encoding', 'X-Accept-Encoding, Accept-Encoding'),
        # The token scan compares case-insensitively, as a field name must
        # be.  With nxt_strncasecmp() swapped for nxt_strncmp() every other
        # case here still passes; this one appends and fails.
        ('accept-encoding', 'accept-encoding'),
        ('a, ACCEPT-ENCODING', 'a, ACCEPT-ENCODING'),
        # "*" varies on everything already; adding to it would say less.
        ('*', '*'),
        # A trailing separator is appended after the last real token, not
        # after the comma: "Origin,," is legal but pointless.
        ('Origin,', 'Origin, Accept-Encoding'),
        ('Origin, ', 'Origin, Accept-Encoding'),
        # Optional whitespace is trimmed before the comparisons, and the
        # value itself is left alone.  "response_headers" is the one source
        # measured to deliver an untrimmed value: it passes the configured
        # string through verbatim, where PHP normalises trailing whitespace
        # before libunit sees it.  Whether any other language module also
        # delivers one is untested, so treat the trim's wider rationale as
        # unverified and this case as the reason it stays.
        #
        # Without the trim around the wildcard test, "* " appends and emits
        # "*, Accept-Encoding", narrowing a header that varied on
        # everything.  The last case is trimmed per token instead, by the
        # "already listed?" scan.
        #
        # The leading-space case also depends on the test client preserving
        # leading OWS in a field value, which some parsers strip; it is
        # asserting Unit's behaviour through a client that happens not to.
        ('* ', '* '),
        (' * ', ' * '),
        ('Accept-Encoding ', 'Accept-Encoding '),
    ],
)
def test_static_compression_vary_merge(temp_dir, configured, expected):
    # "response_headers" runs after the field is generated and replaces or
    # removes a field it names outright.  A response chosen by negotiation
    # varies on Accept-Encoding whatever an operator wrote there, so the
    # merge is re-asserted afterwards; without that, a "Vary: Origin" in the
    # configuration silently drops the coding from every shared cache key.
    action = {"share": f'{temp_dir}/assets$uri'}

    if configured is not None:
        action["response_headers"] = {"Vary": configured}

    assert 'success' in client.conf(action, 'routes/0/action'), 'configure'

    resp = client.get(
        url='/big.css',
        headers={
            'Host': 'localhost',
            'Connection': 'close',
            'Accept-Encoding': 'gzip',
        },
    )

    assert resp['status'] == 200, 'status 200'
    assert resp['headers']['Content-Encoding'] == 'gzip', 'compressed'
    assert resp['headers']['Vary'] == expected, 'merged Vary'


@pytest.mark.parametrize('coding', ['gzip', None])
def test_static_compression_vary_merge_removed(temp_dir, coding):
    # "response_headers" can remove a field outright by setting it to null,
    # and removal is f->skip = 1 rather than an erasure.  The merge scan
    # filters on !f->skip, so a removed Vary is invisible to it: "vary" stays
    # NULL and control reaches the tail that adds a fresh field.  The response
    # therefore holds two Vary fields, one skipped and one new.
    #
    # Assert the emitted header is a single string, not a list.  The test
    # client collapses one occurrence to a str and two to a list, so this
    # fails if the skipped field is ever serialised -- a class of bug this
    # code has hit before, where a skipped response field's slot is reused.
    assert 'success' in client.conf(
        {
            "share": f'{temp_dir}/assets$uri',
            "response_headers": {"Vary": None},
        },
        'routes/0/action',
    ), 'configure'

    headers = {'Host': 'localhost', 'Connection': 'close'}

    if coding is not None:
        headers['Accept-Encoding'] = coding

    resp = client.get(url='/big.css', headers=headers)

    assert resp['status'] == 200, 'status 200'
    assert resp['headers']['Vary'] == 'Accept-Encoding', 're-added'
    assert isinstance(resp['headers']['Vary'], str), 'exactly one Vary'


def test_static_compression_vary_merge_field_name_case(temp_dir):
    # A configured key of "vary" is still merged rather than emitted beside
    # the generated field.
    #
    # Measured limit: this does NOT pin the nxt_strncasecmp(f->name, "Vary")
    # comparison inside nxt_http_comp_merge_vary().  Making that comparison
    # case-sensitive leaves this test passing, because "response_headers"
    # matches the generated field case-insensitively itself and replaces its
    # value, so the field reaching the merge is still named "Vary" whatever
    # the configuration wrote.  Pinning the merge's own field-name comparison
    # needs a header authored by an application, which no test here can do --
    # see the note on the uncovered application path below.
    assert 'success' in client.conf(
        {
            "share": f'{temp_dir}/assets$uri',
            "response_headers": {"vary": "Origin"},
        },
        'routes/0/action',
    ), 'configure'

    resp = client.get(
        url='/big.css',
        headers={
            'Host': 'localhost',
            'Connection': 'close',
            'Accept-Encoding': 'gzip',
        },
    )

    assert resp['status'] == 200, 'status 200'
    assert resp['headers']['Vary'] == 'Origin, Accept-Encoding', 'merged'


@pytest.mark.parametrize(
    ('configured', 'expected'),
    [
        ('Origin', 'Origin, Accept-Encoding'),
        ('*', '*'),
        ('Accept-Encoding', 'Accept-Encoding'),
    ],
)
def test_static_compression_vary_merge_identity(temp_dir, configured, expected):
    # r->resp.vary_accept_encoding is set in nxt_http_comp_check_acceptable()
    # before any coding is chosen, so the merge and the re-assert both run for
    # an identity response too.  The identity response is precisely the one a
    # cache must not reuse for a gzip-capable client, so a merge that only
    # worked on the coded path would leave the hole open from the other side.
    assert 'success' in client.conf(
        {
            "share": f'{temp_dir}/assets$uri',
            "response_headers": {"Vary": configured},
        },
        'routes/0/action',
    ), 'configure'

    resp = client.get(
        url='/big.css',
        headers={'Host': 'localhost', 'Connection': 'close'},
    )

    assert resp['status'] == 200, 'status 200'
    assert 'Content-Encoding' not in resp['headers'], 'not compressed'
    assert resp['headers']['Vary'] == expected, 'merged on identity too'


# Uncovered by design, recorded so the gap is not mistaken for coverage:
# every test above drives the merge through "response_headers" against a
# "share", so the pre-existing Vary is always one Unit itself generated and
# response_headers then replaced.  A Vary authored by an *application* reaches
# nxt_http_comp_merge_vary() by the other caller,
# nxt_router_response_ready_handler(), which copies the app's fields into
# r->resp before nxt_http_comp_check_acceptable() runs.  Any CORS application
# emitting "Vary: Origin" takes that path, and nothing here exercises it --
# including the merge's own case-insensitive field-name match.  Covering it
# needs a language-module test, not a static one.


def _raw_get(url='/big.css', **headers):
    # Raw bytes: the body has to be decompressed, so it must not be decoded.
    raw = client.get(
        url=url,
        headers={'Host': 'localhost', 'Connection': 'close', **headers},
        encoding='latin-1',
        read_buffer_size=1024 * 1024,
        raw_resp=True,
    )
    head, _, body = raw.partition('\r\n\r\n')
    lines = head.split('\r\n')
    status = int(lines[0].split(' ')[1])
    hdrs = dict(line.split(': ', 1) for line in lines[1:])
    body = body.encode('latin-1')

    if hdrs.get('Transfer-Encoding') == 'chunked':
        body = client._parse_chunked_body(body)

    return status, hdrs, body


def test_static_compression_range_identity_refused(temp_dir):
    # A Range is served as identity -- coding a byte slice would compress the
    # wrong bytes -- so a client that sent "identity;q=0" must not be given
    # one: it asked not to receive the file's own bytes and a 206 is exactly
    # those.  The request is still serveable, because it named a coding Unit
    # has, so the Range is dropped and the full 200 is sent in that coding.
    #
    # Without the fix this is a 206 carrying identity bytes to a client that
    # refused identity, which is what #355 reports.
    data = Path(f'{temp_dir}/assets/big.css').read_bytes()

    status, headers, body = _raw_get(
        **{'Accept-Encoding': 'gzip, identity;q=0', 'Range': 'bytes=0-9'}
    )

    assert status == 200, 'the Range is dropped, not the request'
    assert headers.get('Content-Encoding') == 'gzip', 'served as gzip'
    assert 'Content-Range' not in headers, 'no Content-Range on the full 200'
    assert gzip.decompress(body) == data, 'the whole file, correctly coded'


def test_static_compression_identity_refused_below_min_length():
    # "min_length" is 10 and tiny.css is two bytes, so the gzip that made this
    # request serveable is never applied and the fallback is the identity the
    # client refused.  406 is the honest answer, with a Range and without: the
    # Range is not what makes the request unserveable.
    for extra in ({}, {'Range': 'bytes=0-1'}):
        status, headers, _ = _raw_get(
            '/tiny.css',
            **{'Accept-Encoding': 'gzip, identity;q=0', **extra},
        )
        assert status == 406, 'a below-minimum gzip is not available'
        assert 'Content-Encoding' not in headers
        assert 'Content-Range' not in headers


def test_static_compression_identity_refused_without_eligible_coding():
    # The configured compressor applies only to text/css.  This extensionless
    # file has no media type, so no coding will be applied to it and identity
    # is again all that is left -- which this client refused.
    for extra in ({}, {'Range': 'bytes=0-1'}):
        status, headers, _ = _raw_get(
            '/raw',
            **{'Accept-Encoding': 'gzip, identity;q=0', **extra},
        )
        assert status == 406, 'no configured coding can serve this response'
        assert 'Content-Encoding' not in headers
        assert 'Content-Range' not in headers


def test_static_compression_406_varies_on_accept_encoding():
    # A 406 from negotiation depends on Accept-Encoding like any negotiated
    # response.  Without "Vary: Accept-Encoding" a cache that stores error
    # responses may give this 406 to a later client that accepts identity.
    for url, accept in (
        ('/big.css', 'identity;q=0, *;q=0'),  # nothing acceptable
        ('/tiny.css', 'gzip, identity;q=0'),  # gzip below "min_length"
        ('/raw', 'gzip, identity;q=0'),  # media type outside "types"
    ):
        status, headers, _ = _raw_get(url, **{'Accept-Encoding': accept})
        assert status == 406, f'{url} {accept!r}'
        assert headers.get('Vary') == 'Accept-Encoding', f'{url} {accept!r}'


def test_static_compression_min_length_is_per_compressor():
    # "min_length" belongs to the compressor, so the highest-weight coding
    # being below its own threshold says nothing about the next one.  gzip is
    # at 1000 and deflate at 0, and tiny.css is two bytes: it is serveable as
    # deflate.  Selecting gzip on its weight alone and stopping there made
    # the response identity -- and, for a client that refused identity, a
    # 406 for a request that can be satisfied.
    assert 'success' in client.conf(
        {
            "types": ["text/css"],
            "compressors": [
                {"encoding": "gzip", "min_length": 1000},
                {"encoding": "deflate", "min_length": 0},
            ],
        },
        'settings/http/compression',
    ), 'two compressors, different minimums'

    for extra in ({}, {'Accept-Encoding': 'gzip, deflate;q=0.5, identity;q=0'}):
        headers = {'Accept-Encoding': 'gzip, deflate;q=0.5', **extra}

        status, hdrs, body = _raw_get('/tiny.css', **headers)

        assert status == 200, f'deflate can serve it: {headers}'
        assert hdrs.get('Content-Encoding') == 'deflate', 'the eligible coding'
        assert zlib.decompress(body) == b'ok', 'round-trips'

    # The weight order still decides among codings that are all eligible.
    status, hdrs, _ = _raw_get(
        '/big.css', **{'Accept-Encoding': 'gzip, deflate;q=0.5'}
    )

    assert status == 200, 'both are above their minimum here'
    assert hdrs.get('Content-Encoding') == 'gzip', 'the highest weight wins'


def test_static_compression_wildcard_stands_for_every_coding(temp_dir):
    # Sect. 12.5.3: "The asterisk '*' symbol in an Accept-Encoding field
    # matches any available content coding not explicitly listed in the
    # field."  So "*" is not a synonym for identity: it offers every enabled
    # coding the client did not name, at its own weight.
    #
    # Reading it as identity alone left the wildcard unable to select a
    # compressor, so this request -- which refuses identity and accepts
    # everything else -- was answered 406 although gzip was enabled, above
    # its "min_length" and inside "types".
    data = Path(f'{temp_dir}/assets/big.css').read_bytes()

    status, headers, body = _raw_get(
        **{'Accept-Encoding': 'identity;q=0, *;q=1'}
    )

    assert status == 200, 'the wildcard offers gzip'
    assert headers.get('Content-Encoding') == 'gzip', 'served as gzip'
    assert gzip.decompress(body) == data, 'the whole file, correctly coded'

    # A coding the field does name is not matched by the wildcard and keeps
    # its own weight, however low: gzip is listed at 0.1, so 0.1 is what gzip
    # is worth, and with gzip the only compressor it is still the only thing
    # left once identity is refused.
    status, headers, body = _raw_get(
        **{'Accept-Encoding': 'identity;q=0, *;q=0.5, gzip;q=0.1'}
    )

    assert status == 200, 'an explicitly named gzip is still acceptable'
    assert headers.get('Content-Encoding') == 'gzip', 'at its own weight'
    assert gzip.decompress(body) == data

    # An explicitly refused coding is not resurrected by the wildcard either.
    # gzip is out, and what "*" offers here is identity.
    status, headers, body = _raw_get(**{'Accept-Encoding': 'gzip;q=0, *;q=1'})

    assert status == 200, 'the wildcard still offers identity'
    assert 'Content-Encoding' not in headers, 'gzip was refused by name'
    assert body == data

    # A named coding wins a tie with the wildcard -- the wildcard is the
    # weaker statement about a coding the client did not mention.
    for spelling in ('gzip, *', 'gzip;q=0.5, *;q=0.5'):
        status, headers, _ = _raw_get(**{'Accept-Encoding': spelling})

        assert status == 200, f'tie with the wildcard: {spelling!r}'
        assert headers.get('Content-Encoding') == 'gzip', 'the named coding'

    # With no coding named at all the wildcard covers identity too, and
    # identity takes the tie: nothing has to be applied to send it.
    for spelling in ('*', '*;q=1'):
        status, headers, body = _raw_get(**{'Accept-Encoding': spelling})

        assert status == 200, f'the bare wildcard: {spelling!r}'
        assert 'Content-Encoding' not in headers, 'identity takes the tie'
        assert body == data


def test_static_compression_wildcard_skips_inapplicable_coding(temp_dir):
    # The wildcard may only stand for a coding that would really be applied.
    # gzip is the only compressor and tiny.css is below its "min_length", so
    # there is nothing for "*" to offer but the identity this client refused:
    # 406, not a 200 of the bytes it declined.
    status, headers, _ = _raw_get(
        '/tiny.css', **{'Accept-Encoding': 'identity;q=0, *;q=1'}
    )

    assert status == 406, 'a below-minimum gzip is not available to "*"'
    assert 'Content-Encoding' not in headers

    # "min_length" is per compressor, so the wildcard has to look past the
    # first one.  gzip is at 1000 and deflate at 0, and tiny.css is two
    # bytes: "*" offers deflate and the request is serveable.
    assert 'success' in client.conf(
        {
            "types": ["text/css"],
            "compressors": [
                {"encoding": "gzip", "min_length": 1000},
                {"encoding": "deflate", "min_length": 0},
            ],
        },
        'settings/http/compression',
    ), 'two compressors, different minimums'

    status, headers, body = _raw_get(
        '/tiny.css', **{'Accept-Encoding': 'identity;q=0, *;q=1'}
    )

    assert status == 200, 'the wildcard reaches the eligible coding'
    assert headers.get('Content-Encoding') == 'deflate', 'the one that applies'
    assert zlib.decompress(body) == b'ok', 'round-trips'

    # And a coding the wildcard outbids by weight is still chosen over the
    # one the field named lower: deflate at the wildcard's 0.5 beats the
    # explicit gzip at 0.1.
    status, headers, body = _raw_get(
        **{'Accept-Encoding': 'identity;q=0, *;q=0.5, gzip;q=0.1'}
    )

    assert status == 200
    assert headers.get('Content-Encoding') == 'deflate', 'the higher weight'
    assert zlib.decompress(body) == Path(
        f'{temp_dir}/assets/big.css'
    ).read_bytes()


def test_static_compression_wildcard_without_compressors(temp_dir):
    # With nothing enabled, identity is the only available coding, so that is
    # all "*" can ever stand for -- and this client named it as refused.
    assert 'success' in client.conf_delete(
        'settings/http/compression'
    ), 'compression off'

    for spelling in (
        'identity;q=0, *;q=1',
        'identity;q=0, *;q=0.5, gzip;q=0.1',
    ):
        status, headers, _ = _raw_get(**{'Accept-Encoding': spelling})

        assert status == 406, f'"*" has only identity to offer: {spelling!r}'
        assert 'Content-Encoding' not in headers

    # The wildcard still says yes to identity where nothing named it.
    status, headers, body = _raw_get(**{'Accept-Encoding': 'gzip;q=0, *;q=1'})

    assert status == 200, 'identity is what the wildcard offers'
    assert 'Content-Encoding' not in headers
    assert body == Path(f'{temp_dir}/assets/big.css').read_bytes()


def test_static_compression_range_identity_refused_guards(temp_dir):
    # The cases either side of it, which must not move.
    size = Path(f'{temp_dir}/assets/big.css').stat().st_size

    # Identity acceptable: a Range is still a 206 of identity bytes, which is
    # deliberate and is what every other server does.
    status, headers, body = _raw_get(
        **{'Accept-Encoding': 'gzip', 'Range': 'bytes=0-9'}
    )
    assert status == 206, 'gzip alone still gets its partial content'
    assert 'Content-Encoding' not in headers, 'a 206 carries no coding'
    assert headers['Content-Range'] == f'bytes 0-9/{size}'
    assert body == b'body{color'

    # Nothing acceptable at all is 406, whether or not a Range is present.
    # That outranks the Range and is unchanged by this fix.
    for extra in ({}, {'Range': 'bytes=0-9'}):
        status, _, _ = _raw_get(
            **{'Accept-Encoding': 'identity;q=0, *;q=0', **extra}
        )
        assert status == 406, 'unacceptable outranks any Range'

    # Refusing identity without a Range was already correct: full gzip 200.
    status, headers, _ = _raw_get(**{'Accept-Encoding': 'gzip, identity;q=0'})
    assert status == 200
    assert headers.get('Content-Encoding') == 'gzip'

    # An explicitly named identity outranks the wildcard.  The client refused
    # everything it did not name and then named identity as acceptable, so a
    # 206 of identity bytes is exactly what it asked for.  Reading the
    # wildcard as a veto here drops a range the client could take.
    status, headers, body = _raw_get(
        **{
            'Accept-Encoding': 'gzip, identity;q=0.5, *;q=0',
            'Range': 'bytes=0-9',
        }
    )
    assert status == 206, 'an explicit identity;q>0 keeps its range'
    assert 'Content-Encoding' not in headers
    assert headers['Content-Range'] == f'bytes 0-9/{size}'
    assert body == b'body{color'

    # A content coding is a token and tokens are case-insensitive
    # (Sect. 8.4.1), so "Identity;q=0" refuses identity just as "identity;q=0"
    # does.  A case-sensitive compare drops the refusal on the floor and
    # serves the 206 this whole test exists to prevent.
    status, headers, _ = _raw_get(
        **{'Accept-Encoding': 'gzip, Identity;q=0', 'Range': 'bytes=0-9'}
    )
    assert status == 200, 'a mixed-case identity token still refuses'
    assert headers.get('Content-Encoding') == 'gzip'

    # The weight is "('q' / 'Q') '=' qvalue" (Sect. 12.4.2), and an ABNF
    # literal is case-insensitive anyway.  A strstr() for ";q=" alone misses
    # ";Q=", and for identity a missed weight is a missed refusal -- the
    # request gets the 206 of identity bytes it asked not to receive.
    status, headers, _ = _raw_get(
        **{'Accept-Encoding': 'gzip, identity;Q=0', 'Range': 'bytes=0-9'}
    )
    assert status == 200, 'an uppercase Q still refuses'
    assert headers.get('Content-Encoding') == 'gzip'

    # "*" is a tchar, so "*foo" is a legal coding name that Unit does not
    # have -- not the wildcard.  Matching the wildcard on the first byte
    # alone made an unknown coding refuse identity and cost the client a
    # range it could have taken.
    status, headers, body = _raw_get(
        **{'Accept-Encoding': 'gzip, *foo;q=0', 'Range': 'bytes=0-9'}
    )
    assert status == 206, 'an unknown coding is not the wildcard'
    assert 'Content-Encoding' not in headers
    assert headers['Content-Range'] == f'bytes 0-9/{size}'
    assert body == b'body{color'

    # OWS is SP or HTAB (Sect. 5.6.3) and is legal either side of the
    # weight's semicolon.  Stripping only the space left the tab forms
    # unparsed, so the element read as an unknown coding and took its
    # refusal with it.
    for spelling in (
        'gzip, identity;	q=0',
        'gzip, identity	;q=0',
        'gzip, identity; q=0',
    ):
        status, headers, _ = _raw_get(
            **{'Accept-Encoding': spelling, 'Range': 'bytes=0-9'}
        )
        assert status == 200, f'whitespace in the weight: {spelling!r}'
        assert headers.get('Content-Encoding') == 'gzip'

    # Sect. 5.3: a list-valued field may arrive as several lines and must be
    # read as one value joined by commas.  A variable query answers with the
    # first matching field only, so the second line went unread: in this order
    # the refusal was lost and the 206 went out anyway, and in the other order
    # the gzip the client would have taken was never seen and it drew a 406.
    for lines_ae in (['gzip', 'identity;q=0'], ['identity;q=0', 'gzip']):
        status, headers, _ = _raw_get(
            **{'Accept-Encoding': lines_ae, 'Range': 'bytes=0-9'}
        )
        assert status == 200, f'repeated field: {lines_ae!r}'
        assert headers.get('Content-Encoding') == 'gzip'

    # Ignoring the Range is all or nothing.  An unsatisfiable range from a
    # client that refused identity would otherwise draw a 416 whose
    # "Content-Range: bytes */size" reports the size of the very
    # representation it refused.
    status, headers, _ = _raw_get(
        **{'Accept-Encoding': 'gzip, identity;q=0', 'Range': 'bytes=99999-'}
    )
    assert status == 200, 'unsatisfiable range is ignored too'
    assert headers.get('Content-Encoding') == 'gzip'
    assert 'Content-Range' not in headers

    # A client that accepts identity still gets its 416.
    status, headers, _ = _raw_get(
        **{'Accept-Encoding': 'gzip', 'Range': 'bytes=99999-'}
    )
    assert status == 416, 'an ordinary unsatisfiable range is still 416'
    assert headers['Content-Range'] == f'bytes */{size}'


def test_static_compression_malformed_weight(temp_dir):
    # A qvalue is "('0' ['.' 0*3DIGIT]) / ('1' ['.' 0*3('0')])" (Sect.
    # 12.4.2).  strtod() takes no digits at all from "q=" and "q=abc" and
    # reports 0, reads "q=0x0" as hexadecimal and "q=0e0" as an exponent, so
    # every one of these was read as an explicit refusal and answered 406 to
    # a client that refused nothing.
    size = Path(f'{temp_dir}/assets/big.css').stat().st_size

    for weight in ('', 'abc', '0x0', '0e0', 'nan', '.5', '2', '1.5'):
        status, headers, body = _raw_get(
            **{'Accept-Encoding': f'identity;q={weight}'}
        )
        assert status == 200, f'a malformed weight is not a refusal: {weight!r}'
        assert 'Content-Encoding' not in headers
        assert len(body) == size

    # "q=nan" was worse than a refusal on any other coding: a NaN compares
    # false against both range bounds, so the element survived the check, and
    # false against the running best weight, so it was then selected.
    status, headers, body = _raw_get(**{'Accept-Encoding': 'gzip;q=nan'})
    assert status == 200
    assert 'Content-Encoding' not in headers, 'a NaN weight selects nothing'
    assert len(body) == size

    # A well-formed zero still refuses, including a fraction longer than the
    # grammar's three digits: being strict about the digit count would read a
    # refusal as an acceptance, which is the wrong way to be strict.
    for weight in ('0', '0.0', '0.000', '0.0000'):
        status, _, _ = _raw_get(**{'Accept-Encoding': f'identity;q={weight}'})
        assert status == 406, f'a well-formed zero still refuses: {weight!r}'

    # A parameter after the weight does not make the weight malformed.
    status, headers, _ = _raw_get(**{'Accept-Encoding': 'gzip;q=0.5;ext=1'})
    assert status == 200
    assert headers.get('Content-Encoding') == 'gzip', 'q=0.5 then an extension'
