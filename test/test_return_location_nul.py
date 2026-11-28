"""A NUL byte from a variable reaches the Location of "return" as "%00".

nxt_is_complex_uri_encoded() and nxt_encode_complex_uri() pass the first
"?" and the first "#" raw.  A NUL byte after one of them is escaped, like
every other byte below 0x20.  "$arg_x" decodes "%00" to a NUL byte.
"""

import pytest

from unit.applications.proto import ApplicationProto

client = ApplicationProto()


@pytest.fixture(autouse=True)
def setup_method_fixture():
    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [
                {"action": {"return": 301, "location": "/a?$arg_x"}}
            ],
            "applications": {},
        }
    )


def fetch(args):
    try:
        resp = client.get(url=f'/?{args}', read_timeout=5, raw_resp=True)
    except Exception as e:  # pylint: disable=broad-except
        print(f'{args}: error {e!r}')
        return None

    head = resp.split('\r\n\r\n', 1)[0]
    location = [
        line for line in head.split('\r\n') if line.lower().startswith('location:')
    ]
    print(f'{args}: status line {head.split(chr(13))[0]!r}, '
          f'Location {[l.encode() for l in location]!r}')
    return location


def test_return_location_nul():
    location = fetch('x=%00')

    assert location == ['Location: /a?%00'], 'NUL is encoded'


def test_return_location_nul_then_escaped():
    # A NUL byte, then a byte that is escaped.
    location = fetch('x=%00%20')

    assert location == ['Location: /a?%00%20'], 'NUL and space are encoded'


def test_return_location_crlf():
    # CR, LF and NUL after "?" are all escaped.
    location = fetch('x=%0D%0A%00')

    assert location == ['Location: /a?%0D%0A%00'], 'CR, LF, NUL encoded'
