import os
import socket
import time
from email.utils import formatdate, parsedate_to_datetime
from pathlib import Path

import pytest

from unit.applications.proto import ApplicationProto
from unit.utils import waitforfiles


client = ApplicationProto()


@pytest.fixture(autouse=True)
def setup_method_fixture(temp_dir):
    assets_dir = f'{temp_dir}/assets'

    Path(f'{assets_dir}/dir').mkdir(parents=True)
    Path(f'{assets_dir}/index.html').write_text('0123456789', encoding='utf-8')
    Path(f'{assets_dir}/README').write_text('readme', encoding='utf-8')
    Path(f'{assets_dir}/log.log').write_text('[debug]', encoding='utf-8')
    Path(f'{assets_dir}/dir/file').write_text('blah', encoding='utf-8')

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"share": f'{assets_dir}$uri'}}],
            "settings": {
                "http": {
                    "static": {"mime_types": {"text/plain": [".log", "README"]}}
                }
            },
        }
    )


def test_static_index(temp_dir):
    def set_index(index):
        assert 'success' in client.conf(
            {"share": f'{temp_dir}/assets$uri', "index": index},
            'routes/0/action',
        ), 'configure index'

    set_index('README')
    assert client.get()['body'] == 'readme', 'index'

    client.conf_delete('routes/0/action/index')
    assert client.get()['body'] == '0123456789', 'delete index'

    set_index('')
    assert client.get()['status'] == 404, 'index empty'


def test_static_index_nul(temp_dir):
    # "index" is appended to "share" and handed to open() as a NUL-terminated
    # C string, so an embedded NUL truncated the name at the sink: an index of
    # "README\0x" opened "README".  "share" has been guarded against this since
    # it is templated; this is the same sink reached by the other half.
    assert 'error' in client.conf(
        {"share": f'{temp_dir}/assets$uri', "index": "README\u0000x"},
        'routes/0/action',
    ), 'index with null character'

    # An empty index stays valid -- see test_static_index.
    assert 'success' in client.conf(
        {"share": f'{temp_dir}/assets$uri', "index": "README"},
        'routes/0/action',
    ), 'clean index still accepted'

    assert client.get()['body'] == 'readme', 'clean index still served'


def test_static_index_default():
    assert client.get(url='/index.html')['body'] == '0123456789', 'index'
    assert client.get(url='/')['body'] == '0123456789', 'index 2'
    assert client.get(url='//')['body'] == '0123456789', 'index 3'
    assert client.get(url='/.')['body'] == '0123456789', 'index 4'
    assert client.get(url='/./')['body'] == '0123456789', 'index 5'
    assert client.get(url='/?blah')['body'] == '0123456789', 'index vars'
    assert client.get(url='/#blah')['body'] == '0123456789', 'index anchor'
    assert client.get(url='/dir/')['status'] == 404, 'index not found'

    resp = client.get(url='/index.html/')
    assert resp['status'] == 404, 'index not found 2 status'
    assert (
        resp['headers']['Content-Type'] == 'text/html'
    ), 'index not found 2 Content-Type'


def test_static_index_invalid(skip_alert, temp_dir):
    skip_alert(r'failed to apply new conf')

    def check_index(index):
        assert 'error' in client.conf(
            {"share": f'{temp_dir}/assets$uri', "index": index},
            'routes/0/action',
        )

    check_index({})
    check_index(['index.html', '$blah'])


def test_static_large_file(temp_dir):
    file_size = 32 * 1024 * 1024
    with open(f'{temp_dir}/assets/large', 'wb') as f:
        f.seek(file_size - 1)
        f.write(b'\0')

    assert (
        len(client.get(url='/large', read_buffer_size=1024 * 1024)['body'])
        == file_size
    ), 'large file'


def test_static_etag(temp_dir):
    etag = client.get(url='/')['headers']['ETag']
    etag_2 = client.get(url='/README')['headers']['ETag']

    assert etag != etag_2, 'different ETag'
    assert etag == client.get(url='/')['headers']['ETag'], 'same ETag'

    with open(f'{temp_dir}/assets/index.html', 'w', encoding='utf-8') as f:
        f.write('blah')

    assert etag != client.get(url='/')['headers']['ETag'], 'new ETag'


def test_static_accept_ranges():
    resp = client.get(url='/index.html')
    assert resp['status'] == 200, 'plain 200'
    assert resp['headers']['Accept-Ranges'] == 'bytes', 'Accept-Ranges on 200'


def range_get(**headers):
    return client.get(
        url='/index.html',
        headers={'Host': 'localhost', 'Connection': 'close', **headers},
    )


def test_static_range_satisfiable():
    # index.html is the 10-byte "0123456789".
    resp = range_get(Range='bytes=0-4')
    assert resp['status'] == 206, '0-4 is 206'
    assert resp['body'] == '01234', '0-4 body slice'
    assert resp['headers']['Content-Range'] == 'bytes 0-4/10'
    assert resp['headers']['Content-Length'] == '5'
    assert resp['headers']['Accept-Ranges'] == 'bytes'

    resp = range_get(Range='bytes=5-')
    assert resp['status'] == 206, '5- is 206'
    assert resp['body'] == '56789', '5- body slice'
    assert resp['headers']['Content-Range'] == 'bytes 5-9/10'

    resp = range_get(Range='bytes=-5')
    assert resp['status'] == 206, '-5 is 206'
    assert resp['body'] == '56789', '-5 (suffix) body slice'
    assert resp['headers']['Content-Range'] == 'bytes 5-9/10'

    # "0-" (whole file) is a valid, satisfiable single range -> 206.
    resp = range_get(Range='bytes=0-')
    assert resp['status'] == 206, '0- is 206'
    assert resp['body'] == '0123456789', '0- body is the whole file'
    assert resp['headers']['Content-Range'] == 'bytes 0-9/10'

    # Past EOF but still satisfiable: the end clamps to the last byte.
    resp = range_get(Range='bytes=5-9999')
    assert resp['status'] == 206, '5-9999 clamps and is 206'
    assert resp['body'] == '56789', '5-9999 body slice'
    assert resp['headers']['Content-Range'] == 'bytes 5-9/10'


def test_static_range_unsatisfiable():
    resp = range_get(Range='bytes=10-')
    assert resp['status'] == 416, 'start at EOF is 416'
    assert resp['body'] == '', 'no body on 416'
    assert resp['headers']['Content-Range'] == 'bytes */10'

    # "-0" (a zero-length suffix) is unsatisfiable, unlike "0-".
    resp = range_get(Range='bytes=-0')
    assert resp['status'] == 416, '-0 is unsatisfiable'
    assert resp['headers']['Content-Range'] == 'bytes */10'


def test_static_range_empty_file(temp_dir):
    # RFC 9110 Sect. 14.4: no range is satisfiable against a zero-length
    # representation.  The suffix form is the trap: "size - suffix" clamps to
    # 0 while "size - 1" is -1, which produced a 206 carrying the malformed
    # "Content-Range: bytes 0--1/0".
    Path(f'{temp_dir}/assets/empty.txt').write_text('', encoding='utf-8')

    def get(**headers):
        return client.get(
            url='/empty.txt',
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    for value in ['bytes=-5', 'bytes=0-', 'bytes=0-4']:
        resp = get(**{'Range': value})
        assert resp['status'] == 416, f'416 for {value} on an empty file'
        assert (
            resp['headers']['Content-Range'] == 'bytes */0'
        ), f'unsatisfiable Content-Range for {value}'

    resp = get()
    assert resp['status'] == 200, 'no Range still serves the empty file'
    assert resp['body'] == '', 'empty body'


def test_static_range_malformed():
    for value in ['bytes=abc', 'bytes=', 'bytes=5-1', 'notbytes=0-4']:
        resp = range_get(Range=value)
        assert resp['status'] == 200, f'malformed {value!r} -> full 200'
        assert resp['body'] == '0123456789', f'full body for {value!r}'
        assert 'Content-Range' not in resp['headers'], f'no Content-Range for {value!r}'


def test_static_range_multi_ignored():
    # A server may legally ignore a multi-range request; only single ranges
    # are implemented, so this must fall back to a full 200.
    resp = range_get(Range='bytes=0-4,5-9')
    assert resp['status'] == 200, 'multi-range falls back to 200'
    assert resp['body'] == '0123456789', 'full body'
    assert 'Content-Range' not in resp['headers'], 'no Content-Range'


def test_static_range_if_range():
    r = client.get(url='/index.html')
    etag = r['headers']['ETag']
    last_modified = r['headers']['Last-Modified']

    # Matching tag: apply the range.
    resp = range_get(Range='bytes=0-4', **{'If-Range': etag})
    assert resp['status'] == 206, 'If-Range matching tag applies range'
    assert resp['body'] == '01234'

    # Non-matching tag: ignore Range, serve full 200.
    resp = range_get(Range='bytes=0-4', **{'If-Range': '"nope"'})
    assert resp['status'] == 200, 'If-Range mismatched tag ignores range'
    assert resp['body'] == '0123456789'

    # Matching date: apply the range.
    resp = range_get(Range='bytes=0-4', **{'If-Range': last_modified})
    assert resp['status'] == 206, 'If-Range matching date applies range'
    assert resp['body'] == '01234'

    # Non-matching (older) date: ignore Range, serve full 200.
    resp = range_get(
        Range='bytes=0-4',
        **{'If-Range': 'Thu, 01 Jan 2000 00:00:00 GMT'},
    )
    assert resp['status'] == 200, 'If-Range mismatched date ignores range'
    assert resp['body'] == '0123456789'

    # If-Range without Range is meaningless and must not affect the result.
    resp = range_get(**{'If-Range': etag})
    assert resp['status'] == 200, 'If-Range without Range is a no-op'
    assert resp['body'] == '0123456789'


def test_static_range_conditional_wins():
    etag = client.get(url='/index.html')['headers']['ETag']

    # RFC 9110 Sect. 13.2.2: preconditions are evaluated before Range is
    # considered at all -- a 304 (or 412) must win over a Range request.
    resp = range_get(Range='bytes=0-4', **{'If-None-Match': etag})
    assert resp['status'] == 304, '304 wins over Range'
    assert resp['body'] == '', 'no body on 304'
    assert 'Content-Range' not in resp['headers'], 'no Content-Range on 304'

    resp = range_get(Range='bytes=0-4', **{'If-Match': '"nope"'})
    assert resp['status'] == 412, '412 wins over Range'
    assert 'Content-Range' not in resp['headers'], 'no Content-Range on 412'


def test_static_conditional_etag():
    etag = client.get(url='/index.html')['headers']['ETag']

    def get(**headers):
        return client.get(
            url='/index.html',
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    for value in [etag, f'W/{etag}', '*', f'"nope", {etag}']:
        resp = get(**{'If-None-Match': value})
        assert resp['status'] == 304, f'304 for {value}'
        assert resp['body'] == '', f'no body for {value}'
        assert resp['headers']['ETag'] == etag, f'ETag echoed for {value}'
        assert (
            'Content-Length' not in resp['headers']
        ), f'no Content-Length for {value}'

    resp = get(**{'If-None-Match': '"nope"'})
    assert resp['status'] == 200, 'mismatch sends the body'
    assert resp['body'] == '0123456789', 'full body on mismatch'


def test_static_conditional_duplicate_field_lines():
    # RFC 9110 Sect. 5.3: repeated field lines are equivalent to one line
    # holding the comma-separated concatenation.  Keeping only the last line
    # seen refused a legitimate request with 412 when an earlier If-Match
    # line matched.
    etag = client.get(url='/index.html')['headers']['ETag']

    def get(header, values):
        return client.get(
            url='/index.html',
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                header: values,
            },
        )

    for values in [[etag, '"other"'], ['"other"', etag]]:
        assert (
            get('If-None-Match', values)['status'] == 304
        ), f'If-None-Match matches in any line: {values}'
        assert (
            get('If-Match', values)['status'] == 200
        ), f'If-Match matches in any line: {values}'

    assert (
        get('If-None-Match', ['"a"', '"b"'])['status'] == 200
    ), 'no If-None-Match line matches'
    assert (
        get('If-Match', ['"a"', '"b"'])['status'] == 412
    ), 'no If-Match line matches'


def test_static_conditional_if_match():
    etag = client.get(url='/index.html')['headers']['ETag']

    def get(**headers):
        return client.get(
            url='/index.html',
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    resp = get(**{'If-Match': etag})
    assert resp['status'] == 200, 'matching If-Match serves the file'
    assert resp['body'] == '0123456789', 'full body'

    assert get(**{'If-Match': '*'})['status'] == 200, '* matches'
    assert get(**{'If-Match': '"nope"'})['status'] == 412, 'mismatch is 412'

    # RFC 9110 Sect. 13.1.1: If-Match uses the strong comparison function,
    # so a weak tag never matches even when the opaque tags are equal.
    assert get(**{'If-Match': f'W/{etag}'})['status'] == 412, 'weak tag'

    # Sect. 13.2.2 evaluation order: If-Match is checked before
    # If-None-Match, so a failed If-Match is a 412, not a 304.
    resp = get(**{'If-Match': '"nope"', 'If-None-Match': etag})
    assert resp['status'] == 412, 'If-Match evaluated first'


def test_static_conditional_unmodified_since():
    def get(value, **extra):
        return client.get(
            url='/index.html',
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'If-Unmodified-Since': value,
                **extra,
            },
        )

    assert get('Thu, 01 Jan 2100 00:00:00 GMT')['status'] == 200, 'unmodified'
    assert get('Thu, 01 Jan 2000 00:00:00 GMT')['status'] == 412, 'modified'
    assert get('not-a-date')['status'] == 200, 'unparsable date is ignored'

    etag = client.get(url='/index.html')['headers']['ETag']

    # If-Match takes precedence over If-Unmodified-Since.
    resp = get('Thu, 01 Jan 2000 00:00:00 GMT', **{'If-Match': etag})
    assert resp['status'] == 200, 'If-Match wins'


def test_static_conditional_modified_since(temp_dir):
    last_modified = client.get(url='/index.html')['headers']['Last-Modified']

    def get(value, **extra):
        return client.get(
            url='/index.html',
            headers={
                'Host': 'localhost',
                'Connection': 'close',
                'If-Modified-Since': value,
                **extra,
            },
        )

    resp = get('Thu, 01 Jan 2100 00:00:00 GMT')
    assert resp['status'] == 304, 'not modified since a future date'
    assert resp['body'] == '', 'no body'

    resp = get(last_modified)
    assert resp['status'] == 304, 'not modified since its own mtime'

    resp = get('Thu, 01 Jan 2000 00:00:00 GMT')
    assert resp['status'] == 200, 'modified since an old date'
    assert resp['body'] == '0123456789', 'full body'

    resp = get('not-a-date')
    assert resp['status'] == 200, 'unparsable date is ignored'

    # RFC 9110 Sect. 13.2.2: a present If-None-Match wins outright, even when
    # it does not match, so this must send the body despite the future date.
    resp = get('Thu, 01 Jan 2100 00:00:00 GMT', **{'If-None-Match': '"nope"'})
    assert resp['status'] == 200, 'If-None-Match takes precedence'
    assert resp['body'] == '0123456789', 'full body'


def test_static_last_modified_is_gmt(temp_dir):
    # RFC 9110 Sect. 5.6.7: an HTTP-date is GMT.  Last-Modified used to be
    # rendered from localtime() and then labelled "GMT", so on a host not set
    # to UTC the value was wrong by the UTC offset while still looking valid.
    # Only a downstream cache comparing it would ever notice.
    mtime = 1600000000  # Sun, 13 Sep 2020 12:26:40 GMT

    # Under UTC the two renderings are identical and this proves nothing, so
    # say so out loud rather than passing.  glibc also treats an unknown zone
    # as UTC, which is how a runner image without tzdata would go green
    # against a broken build; the workflow pins a zone for this reason.
    if time.localtime(mtime).tm_gmtoff == 0:
        pytest.skip('TZ is UTC: localtime() and gmtime() cannot be told apart')

    os.utime(f'{temp_dir}/assets/index.html', (mtime, mtime))

    last_modified = client.get(url='/index.html')['headers']['Last-Modified']

    assert last_modified == formatdate(
        mtime, usegmt=True
    ), 'Last-Modified is the GMT rendering of mtime'


def test_static_last_modified_pre_epoch(temp_dir):
    # A pre-epoch mtime is legal ("touch -d 1969-07-20").  nxt_gmtime() used
    # to take the time of day from an unsigned "s % 86400", which wraps for a
    # negative time: the header came out as "Thu, 01 Jan 1970 1193046:28:1",
    # malformed, with the " GMT" truncated off the end of the buffer.
    path = f'{temp_dir}/assets/index.html'
    mtime = -14182940  # Sun, 20 Jul 1969 20:17:40 GMT

    try:
        os.utime(path, (mtime, mtime))
    except (OSError, OverflowError):
        pytest.skip('filesystem rejects a pre-epoch mtime')

    if os.stat(path).st_mtime != mtime:
        pytest.skip('filesystem does not keep a pre-epoch mtime')

    last_modified = client.get(url='/index.html')['headers']['Last-Modified']

    # Checked separately from the equality below: this is the truncation,
    # and it is worth failing on its own terms.
    assert last_modified.endswith(' GMT'), 'well-formed pre-epoch date'

    # The exact string, not a parsed timestamp: parsedate_to_datetime()
    # discards the day name, so a wrong weekday would parse to the right
    # instant.  This mtime is 164 days pre-epoch, far enough that a weekday
    # taken from an unsigned day number comes out wrong.
    assert last_modified == formatdate(
        mtime, usegmt=True
    ), 'pre-epoch mtime is reported as itself, not clamped or wrapped'


def test_static_redirect():
    resp = client.get(url='/dir')
    assert resp['status'] == 301, 'redirect status'
    assert resp['headers']['Location'] == '/dir/', 'redirect Location'
    assert 'Content-Type' not in resp['headers'], 'redirect Content-Type'


def test_static_space_in_name(temp_dir):
    assets_dir = f'{temp_dir}/assets'

    Path(f'{assets_dir}/dir/file').rename(f'{assets_dir}/dir/fi le')

    assert waitforfiles(f'{assets_dir}/dir/fi le')
    assert client.get(url='/dir/fi le')['body'] == 'blah', 'file name'

    Path(f'{assets_dir}/dir').rename(f'{assets_dir}/di r')
    assert waitforfiles(f'{assets_dir}/di r/fi le')
    assert client.get(url='/di r/fi le')['body'] == 'blah', 'dir name'

    Path(f'{assets_dir}/di r').rename(f'{assets_dir}/ di r ')
    assert waitforfiles(f'{assets_dir}/ di r /fi le')
    assert (
        client.get(url='/ di r /fi le')['body'] == 'blah'
    ), 'dir name enclosing'

    assert (
        client.get(url='/%20di%20r%20/fi le')['body'] == 'blah'
    ), 'dir encoded'
    assert client.get(url='/ di r %2Ffi le')['body'] == 'blah', 'slash encoded'
    assert client.get(url='/ di r /fi%20le')['body'] == 'blah', 'file encoded'
    assert (
        client.get(url='/%20di%20r%20%2Ffi%20le')['body'] == 'blah'
    ), 'encoded'
    assert (
        client.get(url='/%20%64%69%20%72%20%2F%66%69%20%6C%65')['body']
        == 'blah'
    ), 'encoded 2'

    Path(f'{assets_dir}/ di r /fi le').rename(f'{assets_dir}/ di r / fi le ')
    assert waitforfiles(f'{assets_dir}/ di r / fi le ')
    assert (
        client.get(url='/%20di%20r%20/%20fi%20le%20')['body'] == 'blah'
    ), 'file name enclosing'

    try:
        Path(f'{temp_dir}/ф а').touch()
        utf8 = True

    except KeyboardInterrupt:
        raise

    except:
        utf8 = False

    if utf8:
        Path(f'{assets_dir}/ di r / fi le ').rename(
            f'{assets_dir}/ di r /фа йл'
        )
        assert waitforfiles(f'{assets_dir}/ di r /фа йл')
        assert client.get(url='/ di r /фа йл')['body'] == 'blah'

        Path(f'{assets_dir}/ di r ').rename(f'{assets_dir}/ди ректория')
        assert waitforfiles(f'{assets_dir}/ди ректория/фа йл')
        assert (
            client.get(url='/ди ректория/фа йл')['body'] == 'blah'
        ), 'dir name 2'


def test_static_unix_socket(temp_dir):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(f'{temp_dir}/assets/unix_socket')

    assert client.get(url='/unix_socket')['status'] == 404, 'socket'

    sock.close()


def test_static_unix_fifo(temp_dir):
    os.mkfifo(f'{temp_dir}/assets/fifo')

    assert client.get(url='/fifo')['status'] == 404, 'fifo'


def test_static_method():
    resp = client.head()
    assert resp['status'] == 200, 'HEAD status'
    assert resp['body'] == '', 'HEAD empty body'

    assert client.delete()['status'] == 405, 'DELETE'
    assert client.post()['status'] == 405, 'POST'
    assert client.put()['status'] == 405, 'PUT'


def test_static_path():
    assert client.get(url='/dir/../dir/file')['status'] == 200, 'relative'

    assert client.get(url='./')['status'] == 400, 'path invalid'
    assert client.get(url='../')['status'] == 400, 'path invalid 2'
    assert client.get(url='/..')['status'] == 400, 'path invalid 3'
    assert client.get(url='../assets/')['status'] == 400, 'path invalid 4'
    assert client.get(url='/../assets/')['status'] == 400, 'path invalid 5'


def test_static_path_encoded():
    # Percent-encoded and partially-encoded dot segments are decoded before
    # path normalization, so an encoded "../" cannot bypass the traversal
    # guard that rejects the plain form.
    assert (
        client.get(url='/dir/%2e%2e/dir/file')['status'] == 200
    ), 'encoded relative stays in root'

    assert client.get(url='/%2e%2e/')['status'] == 400, 'encoded ..'
    assert client.get(url='/%2e%2e')['status'] == 400, 'encoded .. no slash'
    assert client.get(url='/%2e./')['status'] == 400, 'partial encoded .. 1'
    assert client.get(url='/.%2e/')['status'] == 400, 'partial encoded .. 2'
    assert (
        client.get(url='/%2e%2e/assets/')['status'] == 400
    ), 'encoded traversal to sibling'


def test_static_two_clients():
    sock = client.get(no_recv=True)
    sock2 = client.get(no_recv=True)

    assert sock.recv(1) == b'H', 'client 1'
    assert sock2.recv(1) == b'H', 'client 2'
    assert sock.recv(1) == b'T', 'client 1 again'
    assert sock2.recv(1) == b'T', 'client 2 again'

    sock.close()
    sock2.close()


def test_static_mime_types():
    assert 'success' in client.conf(
        {
            "text/x-code/x-blah/x-blah": "readme",
            "text/plain": [".html", ".log", "file"],
        },
        'settings/http/static/mime_types',
    ), 'configure mime_types'

    assert (
        client.get(url='/README')['headers']['Content-Type']
        == 'text/x-code/x-blah/x-blah'
    ), 'mime_types string case insensitive'
    assert (
        client.get(url='/index.html')['headers']['Content-Type'] == 'text/plain'
    ), 'mime_types html'
    assert (
        client.get(url='/')['headers']['Content-Type'] == 'text/plain'
    ), 'mime_types index default'
    assert (
        client.get(url='/dir/file')['headers']['Content-Type'] == 'text/plain'
    ), 'mime_types file in dir'


def test_static_mime_types_partial_match():
    assert 'success' in client.conf(
        {
            "text/x-blah": ["ile", "fil", "f", "e", ".file"],
        },
        'settings/http/static/mime_types',
    ), 'configure mime_types'
    assert 'Content-Type' not in client.get(url='/dir/file'), 'partial match'


def test_static_mime_types_reconfigure():
    assert 'success' in client.conf(
        {
            "text/x-code": "readme",
            "text/plain": [".html", ".log", "file"],
        },
        'settings/http/static/mime_types',
    ), 'configure mime_types'

    assert client.conf_get('settings/http/static/mime_types') == {
        'text/x-code': 'readme',
        'text/plain': ['.html', '.log', 'file'],
    }, 'mime_types get'
    assert (
        client.conf_get('settings/http/static/mime_types/text%2Fx-code')
        == 'readme'
    ), 'mime_types get string'
    assert client.conf_get('settings/http/static/mime_types/text%2Fplain') == [
        '.html',
        '.log',
        'file',
    ], 'mime_types get array'
    assert (
        client.conf_get('settings/http/static/mime_types/text%2Fplain/1')
        == '.log'
    ), 'mime_types get array element'

    assert 'success' in client.conf_delete(
        'settings/http/static/mime_types/text%2Fplain/2'
    ), 'mime_types remove array element'
    assert (
        'Content-Type' not in client.get(url='/dir/file')['headers']
    ), 'mime_types removed'

    assert 'success' in client.conf_post(
        '"file"', 'settings/http/static/mime_types/text%2Fplain'
    ), 'mime_types add array element'
    assert (
        client.get(url='/dir/file')['headers']['Content-Type'] == 'text/plain'
    ), 'mime_types reverted'

    assert 'success' in client.conf(
        '"file"', 'settings/http/static/mime_types/text%2Fplain'
    ), 'configure mime_types update'
    assert (
        client.get(url='/dir/file')['headers']['Content-Type'] == 'text/plain'
    ), 'mime_types updated'
    assert (
        'Content-Type' not in client.get(url='/log.log')['headers']
    ), 'mime_types updated 2'

    assert 'success' in client.conf(
        '".log"', 'settings/http/static/mime_types/text%2Fblahblahblah'
    ), 'configure mime_types create'
    assert (
        client.get(url='/log.log')['headers']['Content-Type']
        == 'text/blahblahblah'
    ), 'mime_types create'


def test_static_mime_types_correct():
    assert 'error' in client.conf(
        {"text/x-code": "readme", "text/plain": "readme"},
        'settings/http/static/mime_types',
    ), 'mime_types same extensions'
    assert 'error' in client.conf(
        {"text/x-code": [".h", ".c"], "text/plain": ".c"},
        'settings/http/static/mime_types',
    ), 'mime_types same extensions array'
    assert 'error' in client.conf(
        {
            "text/x-code": [".h", ".c", "readme"],
            "text/plain": "README",
        },
        'settings/http/static/mime_types',
    ), 'mime_types same extensions case insensitive'


@pytest.mark.skip('not yet')
def test_static_mime_types_invalid(temp_dir):
    assert 'error' in client.http(
        b"""PUT /config/settings/http/static/mime_types/%0%00% HTTP/1.1\r
Host: localhost\r
Connection: close\r
Content-Length: 6\r
\r
\"blah\"""",
        raw_resp=True,
        raw=True,
        sock_type='unix',
        addr=f'{temp_dir}/control.unit.sock',
    ), 'mime_types invalid'


def test_static_buffer_reuse():
    # Exercise the static-file buffer-descriptor freelist: many keep-alive
    # requests over a single connection recycle the same descriptor (allocated
    # on send, returned to the thread-local freelist on completion), so a
    # corrupted recycle would surface as a wrong or truncated body.
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.connect(('127.0.0.1', 8080))
    sock.settimeout(5)

    def one_request():
        sock.sendall(
            b'GET / HTTP/1.1\r\nHost: localhost\r\n'
            b'Connection: keep-alive\r\n\r\n'
        )
        data = b''
        while b'\r\n\r\n' not in data:
            data += sock.recv(4096)
        head, _, rest = data.partition(b'\r\n\r\n')
        length = int(
            dict(
                line.split(': ', 1)
                for line in head.decode().split('\r\n')[1:]
            )['Content-Length']
        )
        while len(rest) < length:
            rest += sock.recv(4096)
        return rest[:length]

    try:
        for i in range(50):
            assert one_request() == b'0123456789', f'keep-alive request {i}'
    finally:
        sock.close()


# ---------------------------------------------------------------------------
# Adversarial probes for the conditional-request and byte-range features.
# Every 206 asserts its Content-Range, every 416 too: the earlier suite passed
# while a suffix range against an empty file emitted "bytes 0--1/0".
# ---------------------------------------------------------------------------


def _raw(url, header_lines, method='GET'):
    """Send a request built from literal header lines (so duplicated header
    fields and odd bytes survive) and return the parsed response."""
    req = f'{method} {url} HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n'
    req = req.encode()
    for line in header_lines:
        if isinstance(line, str):
            line = line.encode()
        req += line + b'\r\n'
    req += b'\r\n'

    return client.http(req, raw=True, encoding='latin-1')


def test_static_range_header_syntax():
    # index.html is the 10-byte "0123456789".
    #
    # RFC 9110 Sect. 14.2: a server MAY ignore Range entirely, so a 200 is
    # always legal; what is not legal is a 206 whose Content-Range and body
    # disagree, or a 416 for a satisfiable range.
    def check(value, body=None, crange=None):
        resp = range_get(Range=value)

        if body is None:
            assert resp['status'] == 200, f'{value!r} -> 200'
            assert resp['body'] == '0123456789', f'{value!r} full body'
            assert 'Content-Range' not in resp['headers'], value
            return

        assert resp['status'] == 206, f'{value!r} -> 206'
        assert resp['body'] == body, f'{value!r} body'
        assert resp['headers']['Content-Range'] == crange, f'{value!r} range'
        assert resp['headers']['Content-Length'] == str(len(body)), value

    # Sect. 14.1: range-unit names are case-insensitive.
    check('BYTES=0-4', '01234', 'bytes 0-4/10')
    check('Bytes=0-4', '01234', 'bytes 0-4/10')

    # Exactly one byte, first and last positions.
    check('bytes=0-0', '0', 'bytes 0-0/10')
    check('bytes=9-9', '9', 'bytes 9-9/10')
    check('bytes=9-', '9', 'bytes 9-9/10')
    check('bytes=-1', '9', 'bytes 9-9/10')

    # Whole file, several spellings; the end clamps to size-1 (Sect. 14.1.2).
    check('bytes=0-9', '0123456789', 'bytes 0-9/10')
    check('bytes=0-10', '0123456789', 'bytes 0-9/10')
    check('bytes=-10', '0123456789', 'bytes 0-9/10')
    check('bytes=-11', '0123456789', 'bytes 0-9/10')
    check('bytes=-99999', '0123456789', 'bytes 0-9/10')

    # Leading zeros are plain decimal digits (1*DIGIT).
    check('bytes=000-004', '01234', 'bytes 0-4/10')
    check('bytes=-05', '56789', 'bytes 5-9/10')

    # The grammar has no OWS after "=" or around "-": ignoring is the
    # RFC-blessed way out (Sect. 14.2), so a 200 is expected and a 206 must
    # be self-consistent.
    for value in ['bytes= 0-4', 'bytes=0 - 4', 'bytes=0-4 ', ' bytes=0-4',
                  'bytes =0-4']:
        resp = range_get(Range=value)
        assert resp['status'] in (200, 206), f'{value!r} status'
        if resp['status'] == 206:
            assert resp['body'] == '01234', f'{value!r} body'
            assert resp['headers']['Content-Range'] == 'bytes 0-4/10', value
        else:
            assert resp['body'] == '0123456789', f'{value!r} full body'

    # No unit, unknown unit, empty value, garbage.
    for value in ['0-4', '=0-4', 'items=0-4', '', 'bytes', 'bytes=-',
                  'bytes=--5', 'bytes=0--4', 'bytes=0-4-', 'bytes=+0-4',
                  'bytes=-+4', 'bytes=0x0-4', 'bytes=,0-4', 'bytes=0-4,']:
        check(value)


def test_static_range_last_byte_boundaries():
    # start == size-1 is the last satisfiable start, start == size is not.
    resp = range_get(Range='bytes=9-100')
    assert resp['status'] == 206
    assert resp['body'] == '9'
    assert resp['headers']['Content-Range'] == 'bytes 9-9/10'

    resp = range_get(Range='bytes=10-10')
    assert resp['status'] == 416, 'start == size is unsatisfiable'
    assert resp['headers']['Content-Range'] == 'bytes */10'
    assert resp['headers']['Content-Length'] == '0'

    resp = range_get(Range='bytes=11-')
    assert resp['status'] == 416
    assert resp['headers']['Content-Range'] == 'bytes */10'


def test_static_range_one_byte_file(temp_dir):
    Path(f'{temp_dir}/assets/one').write_text('x', encoding='utf-8')

    def get(**headers):
        return client.get(
            url='/one',
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    for value in ['bytes=0-0', 'bytes=0-', 'bytes=-1', 'bytes=-2',
                  'bytes=0-5']:
        resp = get(Range=value)
        assert resp['status'] == 206, f'{value} on 1-byte file'
        assert resp['body'] == 'x', value
        assert resp['headers']['Content-Range'] == 'bytes 0-0/1', value
        assert resp['headers']['Content-Length'] == '1', value

    for value in ['bytes=1-', 'bytes=1-1', 'bytes=-0']:
        resp = get(Range=value)
        assert resp['status'] == 416, f'{value} on 1-byte file'
        assert resp['headers']['Content-Range'] == 'bytes */1', value
        assert resp['body'] == ''


def _check_range_honest(value, resp, size=10, content='0123456789'):
    """A Range request may be answered 200 (ignored), 416 (unsatisfiable) or
    206 -- but a 206 must carry a well-formed Content-Range whose slice is
    exactly the body."""
    status = resp['status']
    assert status in (200, 206, 416), f'{value!r} -> {status}'

    if status == 200:
        assert resp['body'] == content, value
        assert 'Content-Range' not in resp['headers'], value

    elif status == 416:
        assert resp['headers']['Content-Range'] == f'bytes */{size}', value
        assert resp['body'] == ''

    else:
        crange = resp['headers']['Content-Range']
        assert crange.startswith('bytes ') and '--' not in crange, (
            f'{value!r}: malformed Content-Range {crange!r}'
        )
        first, last = crange[6:].split('/')[0].split('-')
        first, last = int(first), int(last)
        assert 0 <= first <= last < size, f'{value!r}: {crange}'
        assert resp['body'] == content[first : last + 1], value
        assert resp['headers']['Content-Length'] == str(last - first + 1), (
            value
        )


def test_static_range_odd_bytes():
    # Control bytes inside the value: either the parser rejects the message
    # (400) or the range is ignored/served honestly.  Nothing may hang or
    # emit a 206 whose slice does not match its Content-Range.
    for value in [b'bytes=0-4\x00', b'bytes=\x000-4', b'bytes=0\x00-4',
                  b'bytes=0-4\x01', b'bytes=0-\x7f4', b'bytes=0-4\x0b',
                  b'bytes=0-4\x0c', b'bytes=\xff0-4', b'bytes=0-4\r',
                  b'bytes=0-4\t', b'\tbytes=0-4']:
        resp = _raw('/index.html', [b'Range: ' + value])
        status = resp['status']
        assert status in (200, 206, 400), f'{value!r} -> {status}'
        if status == 206:
            assert resp['headers']['Content-Range'] == 'bytes 0-4/10', value
            assert resp['body'] == '01234', value
        elif status == 200:
            assert resp['body'] == '0123456789', value

    # obs-fold continuation: either 400 or spliced as a single space.
    resp = _raw('/index.html', ['Range: bytes=0-4', ' ,5-9'])
    assert resp['status'] in (200, 206, 400)
    if resp['status'] == 206:
        assert resp['headers']['Content-Range'] == 'bytes 0-4/10'
        assert resp['body'] == '01234'


def test_static_range_duplicate_headers():
    # Range is not a list-based field (RFC 9110 Sect. 14.2), so two Range
    # lines make an invalid message: reject (400) or ignore (200).  Serving a
    # 206 for one of them is tolerated as long as it is self-consistent.
    resp = _raw('/index.html', ['Range: bytes=0-4', 'Range: bytes=5-9'])
    assert resp['status'] in (200, 206, 400)
    if resp['status'] == 206:
        crange = resp['headers']['Content-Range']
        assert crange in ('bytes 0-4/10', 'bytes 5-9/10'), crange
        expected = '01234' if crange == 'bytes 0-4/10' else '56789'
        assert resp['body'] == expected

    # Two If-Range lines, one matching, one not: the field is a single
    # validator, so this is invalid; whichever is picked, the outcome must be
    # a coherent 200 or 206.
    etag = client.get(url='/index.html')['headers']['ETag']
    resp = _raw('/index.html', ['Range: bytes=0-4', f'If-Range: {etag}',
                                'If-Range: "nope"'])
    assert resp['status'] in (200, 206, 400)
    if resp['status'] == 206:
        assert resp['headers']['Content-Range'] == 'bytes 0-4/10'
        assert resp['body'] == '01234'


def test_static_conditional_duplicate_list_headers():
    # If-None-Match and If-Match are list-based (#entity-tag), and RFC 9110
    # Sect. 5.3 lets a sender split a list across field lines; the recipient
    # MUST treat them as the one combined list.  The matching tag on the
    # FIRST line must therefore count.
    etag = client.get(url='/index.html')['headers']['ETag']

    # Regression: only the last If-None-Match line used to be consulted.
    resp = _raw('/index.html', [f'If-None-Match: {etag}',
                                'If-None-Match: "other"'])
    assert resp['status'] == 304, 'matching tag on the first INM line'

    resp = _raw('/index.html', ['If-None-Match: "other"',
                                f'If-None-Match: {etag}'])
    assert resp['status'] == 304, 'matching tag on the last INM line'

    # Regression: only the last If-Match line used to be consulted, so a
    # matching tag on the first line was lost and the request was refused.
    resp = _raw('/index.html', [f'If-Match: {etag}', 'If-Match: "other"'])
    assert resp['status'] == 200, 'matching tag on the first IM line'
    assert resp['body'] == '0123456789'

    resp = _raw('/index.html', ['If-Match: "other"', f'If-Match: {etag}'])
    assert resp['status'] == 200, 'matching tag on the last IM line'


def test_static_conditional_header_syntax():
    etag = client.get(url='/index.html')['headers']['ETag']
    opaque = etag.strip('"')

    def get(**headers):
        return client.get(
            url='/index.html',
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    # Field names are case-insensitive.
    assert get(**{'if-none-match': etag})['status'] == 304
    assert get(**{'IF-NONE-MATCH': etag})['status'] == 304
    assert get(**{'if-match': '"nope"'})['status'] == 412
    assert get(**{'RANGE': 'bytes=0-4'})['status'] == 206

    # OWS and list punctuation around the tag (Sect. 5.6.1).
    for value in [f' {etag}', f'{etag} ', f'\t{etag}', f'"x",{etag}',
                  f'"x", {etag}', f'{etag},"x"', f'"x" , {etag} , "y"',
                  f',{etag}', f'{etag},', f',,{etag},,']:
        assert get(**{'If-None-Match': value})['status'] == 304, repr(value)

    # Weak tag vs. weak comparison (INM) and strong comparison (IM).
    assert get(**{'If-None-Match': f'W/{etag}'})['status'] == 304
    assert get(**{'If-None-Match': f'"x", W/{etag}'})['status'] == 304
    assert get(**{'If-Match': f'W/{etag}'})['status'] == 412
    assert get(**{'If-Match': f'{etag}, W/"x"'})['status'] == 200

    # "*" only as the entire field value.
    assert get(**{'If-None-Match': '*'})['status'] == 304
    assert get(**{'If-None-Match': ' * '})['status'] == 304
    assert get(**{'If-Match': '*'})['status'] == 200
    assert get(**{'If-Match': '*, "x"'})['status'] == 412
    assert get(**{'If-None-Match': '"x", *'})['status'] == 200
    assert get(**{'If-None-Match': '**'})['status'] == 200
    assert get(**{'If-None-Match': '"*"'})['status'] == 200

    # Tags that merely contain or are contained by ours must not match.
    for value in [opaque, f'"{opaque}x"', f'"x{opaque}"', f'"{opaque[:-1]}"',
                  f'"{opaque.upper()}"', f'"{etag}']:
        assert get(**{'If-None-Match': value})['status'] == 200, repr(value)

    # Unterminated quote must not run into a following list element.
    assert get(**{'If-None-Match': f'"x, {etag}'})['status'] == 200

    # Empty values: If-None-Match "" matches nothing -> 200.
    assert get(**{'If-None-Match': ''})['status'] == 200
    assert get(**{'If-Range': ''})['status'] == 200

    # A long list (just under the 8 KiB header limit) still finds the tag
    # at the end / never matches.
    long_list = ', '.join(['"%08d"' % i for i in range(500)])
    assert get(**{'If-None-Match': long_list + ', ' + etag})['status'] == 304
    assert get(**{'If-None-Match': long_list})['status'] == 200


def test_static_conditional_dates(temp_dir):
    mtime = int(time.time()) - 86400 * 3
    os.utime(f'{temp_dir}/assets/index.html', (mtime, mtime))

    def get(**headers):
        return client.get(
            url='/index.html',
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    last_modified = get()['headers']['Last-Modified']
    dt = parsedate_to_datetime(last_modified)

    # All three HTTP-date formats (Sect. 5.6.7) name the same instant.
    rfc850 = dt.strftime('%A, %d-%b-%y %H:%M:%S GMT')
    asctime = dt.strftime('%a %b ') + f'{dt.day:2d}' + dt.strftime(
        ' %H:%M:%S %Y'
    )

    for value in [last_modified, rfc850, asctime]:
        assert get(**{'If-Modified-Since': value})['status'] == 304, value
        assert get(**{'If-Unmodified-Since': value})['status'] == 200, value
        r = get(Range='bytes=0-4', **{'If-Range': value})
        assert r['status'] == 206, f'If-Range {value!r} exact'
        assert r['headers']['Content-Range'] == 'bytes 0-4/10', value

    before = formatdate(mtime - 1, usegmt=True)
    after = formatdate(mtime + 1, usegmt=True)

    assert get(**{'If-Modified-Since': before})['status'] == 200
    assert get(**{'If-Modified-Since': after})['status'] == 304
    assert get(**{'If-Unmodified-Since': before})['status'] == 412
    assert get(**{'If-Unmodified-Since': after})['status'] == 200

    # Not an HTTP-date: ignored (Sect. 13.1.3 / 13.1.4), never a 412; as an
    # If-Range it is a mismatch, so Range is ignored (200).
    # (nxt_time_parse() is lenient about the zone token -- " GMT" missing,
    # " XYZ", or trailing junk after a complete date all parse; not probed.)
    for value in ['yesterday', '0', '1700000000', last_modified[:-8], '']:
        assert get(**{'If-Modified-Since': value})['status'] == 200, value
        assert get(**{'If-Unmodified-Since': value})['status'] == 200, value
        r = get(Range='bytes=0-4', **{'If-Range': value})
        assert r['status'] == 200, f'If-Range {value!r} is a mismatch'
        assert 'Content-Range' not in r['headers']

    # If-Range wants an exact date; newer or older is a mismatch.
    for value in [before, after]:
        r = get(Range='bytes=0-4', **{'If-Range': value})
        assert r['status'] == 200, value
        assert r['body'] == '0123456789'

    # A 304 carries the validators and no framing headers.
    r = get(**{'If-Modified-Since': last_modified})
    assert r['status'] == 304
    assert r['headers']['Last-Modified'] == last_modified
    assert 'ETag' in r['headers']
    assert 'Content-Length' not in r['headers']
    assert 'Transfer-Encoding' not in r['headers']


def test_static_conditional_precedence():
    r = client.get(url='/index.html')
    etag = r['headers']['ETag']
    last_modified = r['headers']['Last-Modified']
    old = 'Thu, 01 Jan 2000 00:00:00 GMT'
    future = 'Fri, 01 Jan 2100 00:00:00 GMT'

    def get(**headers):
        return client.get(
            url='/index.html',
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    # Sect. 13.2.2 step 1 -> 2: If-Unmodified-Since is ignored when If-Match
    # is present, whichever way If-Match goes.
    assert get(**{'If-Match': etag, 'If-Unmodified-Since': old})['status'] == 200
    assert get(**{'If-Match': '"x"', 'If-Unmodified-Since': future})[
        'status'
    ] == 412

    # Step 3 -> 4: If-Modified-Since is ignored when If-None-Match is present.
    assert get(**{'If-None-Match': '"x"', 'If-Modified-Since': future})[
        'status'
    ] == 200
    assert get(**{'If-None-Match': etag, 'If-Modified-Since': old})[
        'status'
    ] == 304

    # A failed step 1/2 wins over a would-be 304 from step 3/4.
    assert get(**{'If-Match': '"x"', 'If-None-Match': etag})['status'] == 412
    assert get(**{'If-Unmodified-Since': old, 'If-None-Match': etag})[
        'status'
    ] == 412
    assert get(**{'If-Unmodified-Since': old, 'If-Modified-Since': future})[
        'status'
    ] == 412

    # A passed step 1 followed by a matching step 3 is a 304.
    assert get(**{'If-Match': etag, 'If-None-Match': etag})['status'] == 304
    assert get(**{'If-Match': '*', 'If-None-Match': '*'})['status'] == 304

    # Step 5: If-Range is consulted only after 1-4 pass.
    r = get(Range='bytes=0-4', **{'If-Range': etag, 'If-None-Match': etag})
    assert r['status'] == 304
    assert 'Content-Range' not in r['headers']

    r = get(Range='bytes=0-4', **{'If-Range': etag, 'If-Match': '"x"'})
    assert r['status'] == 412
    assert 'Content-Range' not in r['headers']

    r = get(Range='bytes=0-4', **{'If-Range': last_modified,
                                  'If-Unmodified-Since': old})
    assert r['status'] == 412

    r = get(Range='bytes=0-4', **{'If-Range': last_modified,
                                  'If-Modified-Since': last_modified})
    assert r['status'] == 304

    # Preconditions pass but If-Range fails: full 200, not 206 and not 416.
    r = get(Range='bytes=50-', **{'If-Range': '"x"', 'If-Match': etag})
    assert r['status'] == 200
    assert r['body'] == '0123456789'
    assert 'Content-Range' not in r['headers']

    # Preconditions pass, If-Range matches, range unsatisfiable: 416.
    r = get(Range='bytes=50-', **{'If-Range': etag, 'If-None-Match': '"x"'})
    assert r['status'] == 416
    assert r['headers']['Content-Range'] == 'bytes */10'

    # A weak If-Range tag never matches (strong comparison, Sect. 13.1.5).
    r = get(Range='bytes=0-4', **{'If-Range': f'W/{etag}'})
    assert r['status'] == 200
    assert r['body'] == '0123456789'

    # If-Range that is a list of tags is not the grammar; the only sane
    # readings are "mismatch" (200) or a self-consistent 206.
    r = get(Range='bytes=0-4', **{'If-Range': f'"x", {etag}'})
    assert r['status'] in (200, 206)
    if r['status'] == 206:
        assert r['headers']['Content-Range'] == 'bytes 0-4/10'

    # Every precondition + Range on a 412: no Content-Range.
    for headers in [{'If-Match': '"x"'}, {'If-Unmodified-Since': old}]:
        r = get(Range='bytes=0-4', **headers)
        assert r['status'] == 412, headers
        assert 'Content-Range' not in r['headers'], headers

    # Every precondition + Range on a 304: no Content-Range, no body, and a
    # 304 wins over what would otherwise be a 416.
    for headers in [{'If-None-Match': etag},
                    {'If-Modified-Since': last_modified}]:
        r = get(Range='bytes=0-4', **headers)
        assert r['status'] == 304, headers
        assert 'Content-Range' not in r['headers'], headers
        assert r['body'] == ''
        r = get(Range='bytes=50-', **headers)
        assert r['status'] == 304, f'304 wins over 416 {headers}'
        assert 'Content-Range' not in r['headers'], headers


def test_static_range_head():
    def head(**headers):
        return client.head(
            url='/index.html',
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    resp = head(Range='bytes=2-6')
    assert resp['status'] == 206, 'HEAD honours Range'
    assert resp['body'] == '', 'no body on HEAD'
    assert resp['headers']['Content-Range'] == 'bytes 2-6/10'
    assert resp['headers']['Content-Length'] == '5', 'partial length on HEAD'
    assert resp['headers']['Accept-Ranges'] == 'bytes'

    resp = head(Range='bytes=10-')
    assert resp['status'] == 416
    assert resp['headers']['Content-Range'] == 'bytes */10'
    assert resp['body'] == ''

    etag = client.get(url='/index.html')['headers']['ETag']
    resp = head(Range='bytes=2-6', **{'If-None-Match': etag})
    assert resp['status'] == 304
    assert 'Content-Range' not in resp['headers']

    resp = head(Range='bytes=2-6', **{'If-Match': '"x"'})
    assert resp['status'] == 412


def test_static_range_post_ignored():
    # Range is defined for GET only (Sect. 14.2); static serves GET/HEAD and
    # answers 405 for the rest regardless of any Range header.
    resp = client.post(
        url='/index.html',
        headers={'Host': 'localhost', 'Connection': 'close',
                 'Range': 'bytes=0-4'},
    )
    assert resp['status'] == 405
    assert 'Content-Range' not in resp['headers']


def _pattern(size):
    # Byte i is (i * 7 + i // 251) & 0xff: aperiodic over any 128 KiB
    # window, so a slice that is offset by even one byte, or fetched from
    # the wrong buffer, compares unequal.
    return bytes(((i * 7) + (i // 251)) & 0xFF for i in range(size))


def _range_bytes(url, first, last=None, suffix=None):
    if suffix is not None:
        value = f'bytes=-{suffix}'
    elif last is None:
        value = f'bytes={first}-'
    else:
        value = f'bytes={first}-{last}'

    resp = client.get(
        url=url,
        headers={'Host': 'localhost', 'Connection': 'close', 'Range': value},
        encoding='latin-1',
        read_buffer_size=1024 * 1024,
    )
    resp['body'] = resp['body'].encode('latin-1')
    return resp


def test_static_range_spans_buffers(temp_dir):
    # NXT_HTTP_STATIC_BUF_SIZE is 128 KiB and NXT_HTTP_STATIC_BUF_COUNT
    # buffers are issued per body-handler round; a 300 KiB file makes a
    # mid-buffer start cross two boundaries and a second read round.
    buf = 128 * 1024
    size = 300 * 1024
    data = _pattern(size)
    Path(f'{temp_dir}/assets/big.bin').write_bytes(data)

    cases = [
        (0, size - 1),                    # whole file as a range
        (0, buf - 1),                     # exactly the first buffer
        (0, buf),                         # one past the first buffer
        (1, buf),                         # off by one at both ends
        (buf - 1, buf),                   # straddles the first boundary
        (buf, 2 * buf - 1),               # exactly the second buffer
        (buf + 1, 2 * buf + 1),           # mid-buffer start, two crossings
        (100_000, 200_000),               # arbitrary mid-buffer slice
        (2 * buf + 5, size - 1),          # tail past the second boundary
        (size - 1, size - 1),             # last byte
        (12345, None),                    # open-ended from mid-buffer
        (2 * buf + 17, None),             # open-ended from the last buffer
    ]

    for first, last in cases:
        resp = _range_bytes('/big.bin', first, last)
        end = size - 1 if last is None else min(last, size - 1)
        assert resp['status'] == 206, (first, last)
        assert resp['headers']['Content-Range'] == f'bytes {first}-{end}/{size}'
        assert resp['headers']['Content-Length'] == str(end - first + 1)
        assert len(resp['body']) == end - first + 1, (first, last)
        assert resp['body'] == data[first : end + 1], (
            f'wrong bytes for {first}-{last}'
        )

    for suffix in [1, buf, buf + 1, 2 * buf + 3, size, size + 1]:
        resp = _range_bytes('/big.bin', None, suffix=suffix)
        first = max(size - suffix, 0)
        assert resp['status'] == 206, suffix
        assert resp['headers']['Content-Range'] == (
            f'bytes {first}-{size - 1}/{size}'
        )
        assert resp['body'] == data[first:], f'wrong bytes for -{suffix}'

    # And the plain GET still returns everything, so the comparison base is
    # not itself suspect.
    resp = client.get(url='/big.bin', encoding='latin-1',
                      read_buffer_size=1024 * 1024)
    assert resp['status'] == 200
    assert resp['body'].encode('latin-1') == data


def test_static_range_keepalive_mix(temp_dir):
    # One connection, many requests: 206 / 200 / 416 / 304 in a row recycle
    # the same per-thread buffer descriptor, so a stale file_pos or file_end
    # would show up as a shifted or truncated body.  The 304 and 416 also
    # prove their framing (no body, connection stays usable).
    size = 200 * 1024
    data = _pattern(size)
    Path(f'{temp_dir}/assets/big.bin').write_bytes(data)
    etag = client.get(url='/big.bin')['headers']['ETag']

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.connect(('127.0.0.1', 8080))
    sock.settimeout(5)

    def request(extra=''):
        sock.sendall(
            (
                f'GET /big.bin HTTP/1.1\r\nHost: localhost\r\n'
                f'Connection: keep-alive\r\n{extra}\r\n'
            ).encode()
        )
        raw = b''
        while b'\r\n\r\n' not in raw:
            raw += sock.recv(65536)
        head, _, rest = raw.partition(b'\r\n\r\n')
        lines = head.decode().split('\r\n')
        status = int(lines[0].split(' ')[1])
        headers = dict(line.split(': ', 1) for line in lines[1:])
        length = int(headers.get('Content-Length', 0))
        while len(rest) < length:
            rest += sock.recv(65536)
        assert len(rest) == length, 'no bytes after the declared body'
        return status, headers, rest

    try:
        steps = [
            ('Range: bytes=150000-180000\r\n', 206, data[150000:180001],
             f'bytes 150000-180000/{size}'),
            ('', 200, data, None),
            ('Range: bytes=-10\r\n', 206, data[-10:],
             f'bytes {size - 10}-{size - 1}/{size}'),
            (f'Range: bytes={size}-\r\n', 416, b'', f'bytes */{size}'),
            ('Range: bytes=0-0\r\n', 206, data[:1], f'bytes 0-0/{size}'),
            (f'If-None-Match: {etag}\r\n', 304, b'', None),
            ('Range: bytes=131071-131072\r\n', 206, data[131071:131073],
             f'bytes 131071-131072/{size}'),
            (f'Range: bytes=5-\r\nIf-None-Match: {etag}\r\n', 304, b'', None),
            ('', 200, data, None),
            ('Range: bytes=1-\r\n', 206, data[1:], f'bytes 1-{size - 1}/{size}'),
        ]

        for i in range(3):
            for extra, want_status, want_body, want_range in steps:
                status, headers, body = request(extra)
                assert status == want_status, (i, extra)
                assert body == want_body, (i, extra, len(body))
                if want_range is None:
                    assert 'Content-Range' not in headers, (i, extra)
                else:
                    assert headers['Content-Range'] == want_range, (i, extra)
    finally:
        sock.close()


def test_static_range_concurrent(temp_dir):
    # Many clients pulling different slices of one file at the same time,
    # each connection interleaving its reads with the others so the
    # per-thread descriptor freelist is churned while bodies are in flight.
    # A leaked file_pos would hand one client another client's bytes.
    import threading

    size = 320 * 1024
    data = _pattern(size)
    Path(f'{temp_dir}/assets/big.bin').write_bytes(data)

    ranges = [
        (0, 65535), (65536, 131071), (131072, 196607), (196608, 262143),
        (262144, size - 1), (1, 300000), (131071, 131073), (100, 100),
        (200000, None), (0, None), (size - 1, None), (77777, 233333),
    ]

    errors = []
    barrier = threading.Barrier(len(ranges) * 2)

    def worker(first, last):
        try:
            for _ in range(4):
                barrier.wait(timeout=10)
                resp = _range_bytes('/big.bin', first, last)
                end = size - 1 if last is None else last
                if resp['status'] != 206:
                    errors.append((first, last, resp['status']))
                    continue
                if resp['headers']['Content-Range'] != (
                    f'bytes {first}-{end}/{size}'
                ):
                    errors.append((first, last, resp['headers']['Content-Range']))
                if resp['body'] != data[first : end + 1]:
                    got = resp['body']
                    off = next(
                        (k for k in range(min(len(got), end - first + 1))
                         if got[k] != data[first + k]),
                        min(len(got), end - first + 1),
                    )
                    errors.append((first, last, 'body', len(got), off))
        except Exception as e:  # pylint: disable=broad-except
            errors.append((first, last, repr(e)))

    threads = [threading.Thread(target=worker, args=r) for r in ranges] + [
        threading.Thread(target=worker, args=r) for r in ranges
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=60)

    assert not errors, errors


def test_static_range_share_features(temp_dir):
    assets = f'{temp_dir}/assets'
    etag = client.get(url='/index.html')['headers']['ETag']

    def get(url, **headers):
        return client.get(
            url=url,
            headers={'Host': 'localhost', 'Connection': 'close', **headers},
        )

    # Index: "/" resolves to index.html; Range and validators apply to it.
    r = get('/', Range='bytes=2-4')
    assert r['status'] == 206, 'range on an index'
    assert r['body'] == '234'
    assert r['headers']['Content-Range'] == 'bytes 2-4/10'
    assert get('/', **{'If-None-Match': etag})['status'] == 304
    assert get('/', **{'If-Match': '"x"'})['status'] == 412

    # Directory redirect: a 301 is neither conditional nor partial.
    for headers in [{'Range': 'bytes=0-1'}, {'If-None-Match': '*'},
                    {'If-Match': '"x"'}, {'If-None-Match': etag},
                    {'If-Modified-Since': 'Fri, 01 Jan 2100 00:00:00 GMT'},
                    {'If-Unmodified-Since': 'Thu, 01 Jan 2000 00:00:00 GMT'}]:
        r = get('/dir', **headers)
        assert r['status'] == 301, headers
        assert r['headers']['Location'] == '/dir/', headers
        assert 'Content-Range' not in r['headers'], headers
        assert 'Accept-Ranges' not in r['headers'], headers

    # 404 is not conditional either (Sect. 13.2.1: preconditions apply only
    # when the unconditional response would be 2xx or 412).
    for headers in [{'Range': 'bytes=0-1'}, {'If-None-Match': '*'},
                    {'If-Match': '"x"'}, {'If-Match': '*'}]:
        r = get('/nope', **headers)
        assert r['status'] == 404, headers
        assert 'Content-Range' not in r['headers'], headers

    # types: index.html is text/html; a share restricted to text/plain
    # refuses it (403, as test_static_types.py documents), so Range and
    # validators never get evaluated and the outcome is the same as without.
    assert 'success' in client.conf(
        {"share": f'{assets}$uri', "types": ["text/plain"]},
        'routes/0/action',
    )
    for headers in [{}, {'Range': 'bytes=0-1'}, {'If-None-Match': etag},
                    {'If-Match': '"x"'}]:
        r = get('/index.html', **headers)
        assert r['status'] == 403, headers
        assert 'Content-Range' not in r['headers'], headers
        assert 'Accept-Ranges' not in r['headers'], headers

    r = get('/README', Range='bytes=0-2')
    assert r['status'] == 206, 'type-matched file still supports ranges'
    assert r['body'] == 'rea'
    assert r['headers']['Content-Range'] == 'bytes 0-2/6'
    assert r['headers']['Content-Type'] == 'text/plain'

    # fallback: an unreadable share falls through and the fallback answers,
    # whatever the conditional/range headers were.
    assert 'success' in client.conf(
        {
            "share": f'{assets}/missing$uri',
            "fallback": {"return": 200},
        },
        'routes/0/action',
    )
    for headers in [{'Range': 'bytes=0-1'}, {'If-None-Match': '*'},
                    {'If-Match': '"x"'}]:
        r = get('/index.html', **headers)
        assert r['status'] == 200, headers
        assert 'Content-Range' not in r['headers'], headers

    # fallback to a second share: the first share's 404 carries no
    # partially-built response into the second share's 206.
    assert 'success' in client.conf(
        {
            "share": [f'{assets}/missing$uri', f'{assets}$uri'],
        },
        'routes/0/action',
    )
    r = get('/index.html', Range='bytes=3-5')
    assert r['status'] == 206
    assert r['body'] == '345'
    assert r['headers']['Content-Range'] == 'bytes 3-5/10'
    assert get('/index.html', **{'If-None-Match': etag})['status'] == 304


def test_static_range_file_changes_between_requests(temp_dir):
    # The ETag encodes mtime and size, so If-Range must stop applying once
    # the file is rewritten (Sect. 13.1.5 exists for resumed downloads).
    path = Path(f'{temp_dir}/assets/index.html')
    r = client.get(url='/index.html')
    etag = r['headers']['ETag']
    last_modified = r['headers']['Last-Modified']

    path.write_text('abcdefghijklmnop', encoding='utf-8')
    new_mtime = int(time.time()) + 5
    os.utime(path, (new_mtime, new_mtime))

    r = range_get(Range='bytes=10-', **{'If-Range': etag})
    assert r['status'] == 200, 'stale If-Range tag ignores Range'
    assert r['body'] == 'abcdefghijklmnop'

    r = range_get(Range='bytes=10-', **{'If-Range': last_modified})
    assert r['status'] == 200, 'stale If-Range date ignores Range'

    r = range_get(**{'If-None-Match': etag})
    assert r['status'] == 200, 'stale INM is a miss'

    r = range_get(**{'If-Match': etag})
    assert r['status'] == 412, 'stale If-Match fails'

    new_etag = client.get(url='/index.html')['headers']['ETag']
    r = range_get(Range='bytes=10-', **{'If-Range': new_etag})
    assert r['status'] == 206
    assert r['body'] == 'klmnop'
    assert r['headers']['Content-Range'] == 'bytes 10-15/16'


def test_static_range_gzip(temp_dir):
    # Compression must be skipped for a 206 (a coded slice would be
    # meaningless) but still work for the plain 200 alongside it.  The
    # module keeps global state, so this needs a fresh unitd (#167).
    import gzip

    from unit.option import option

    if not option.restart:
        pytest.skip('needs --restart until #167 is fixed')

    resp = client.conf(
        {
            "http": {
                "compression": {
                    "types": ["text/html*"],
                    "compressors": [{"encoding": "gzip", "level": 1}],
                }
            }
        },
        'settings',
    )
    if 'supported compressor' in resp.get('detail', ''):
        pytest.skip('unit built without gzip support')
    assert 'success' in resp, resp

    size = 200 * 1024
    data = ('0123456789abcdef' * (size // 16)).encode()
    Path(f'{temp_dir}/assets/big.html').write_bytes(data)

    def get(**headers):
        raw = client.get(
            url='/big.html',
            headers={'Host': 'localhost', 'Connection': 'close',
                     'Accept-Encoding': 'gzip', **headers},
            encoding='latin-1',
            read_buffer_size=1024 * 1024,
            raw_resp=True,
        )
        head, _, body = raw.partition('\r\n\r\n')
        lines = head.split('\r\n')
        status = int(lines[0].split(' ')[1])
        headers = dict(line.split(': ', 1) for line in lines[1:])
        body = body.encode('latin-1')
        if headers.get('Transfer-Encoding') == 'chunked':
            body = client._parse_chunked_body(body)
        return status, headers, body

    status, headers, body = get()
    assert status == 200
    assert headers.get('Content-Encoding') == 'gzip', 'plain GET compresses'
    assert gzip.decompress(body) == data

    status, headers, body = get(Range='bytes=100000-200000')
    assert status == 206
    assert 'Content-Encoding' not in headers, 'no coding on a 206'
    assert headers['Content-Range'] == f'bytes 100000-200000/{size}'
    assert headers['Content-Length'] == '100001'
    assert body == data[100000:200001]

    status, headers, body = get(Range=f'bytes={size}-')
    assert status == 416
    assert 'Content-Encoding' not in headers
    assert headers['Content-Range'] == f'bytes */{size}'

    # A 200 after the 206 still compresses: the skip is per request.
    status, headers, body = get()
    assert status == 200
    assert headers.get('Content-Encoding') == 'gzip'


def test_static_range_numeric_overflow():
    # Kept last in the file: the sign-flipped inputs below hang their
    # connection and leak an open descriptor in the router, which the
    # harness's teardown fd check reports and which then breaks every test
    # that would run after it in the same unitd.
    # RFC 9110 Sect. 14.1.2: a first-pos greater than or equal to the current
    # length is unsatisfiable (416), whatever its magnitude.  A value that
    # does not fit nxt_off_t must not wrap into a "valid" offset.
    #
    # 2**63 flips the sign of a 64-bit signed accumulator, 2**64-1 wraps to
    # -1, 2**64 wraps to 0.  Any of these as the first-pos is >= size, so
    # 416 (or 200) is right; a 206 for them is a wrap.
    firsts = ['9223372036854775807', '9223372036854775808',
              '9223372036854775809', '18446744073709551615',
              '18446744073709551616', '18446744073709551617',
              '99999999999999999999', '9' * 40, '9' * 4000]

    for first in firsts:
        for value in [f'bytes={first}-', f'bytes={first}-{first}']:
            resp = range_get(Range=value)
            # Regression: a wrapped negative first-pos used to pass the
            # "a >= size" test and be served as a 206 with a negative
            # Content-Range start, leaking the open file descriptor.
            assert resp['status'] != 206, (
                f'{value[:60]!r}: 206 {resp["headers"].get("Content-Range")}'
            )
            _check_range_honest(value, resp)

    # The same magnitudes as last-pos: satisfiable, must clamp to size-1.
    for last in firsts:
        for first in ['0', '5']:
            value = f'bytes={first}-{last}'
            _check_range_honest(value, range_get(Range=value))

    # And as a suffix-length (Sect. 14.1.2: longer than the representation
    # means the whole representation).
    for suffix in firsts:
        value = f'bytes=-{suffix}'
        _check_range_honest(value, range_get(Range=value))

    # Negative-looking inputs are simply not the grammar.
    for value in ['bytes=-5-9', 'bytes=-1-', 'bytes=5--9', 'bytes=-0-']:
        _check_range_honest(value, range_get(Range=value))
