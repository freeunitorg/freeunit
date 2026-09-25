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
