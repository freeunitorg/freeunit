import re

import pytest

from unit import port as port_map
from unit.applications.lang.perl import ApplicationPerl
from unit.option import option

prerequisites = {'modules': {'perl': 'all'}}

client = ApplicationPerl()


def test_perl_application(date_to_sec_epoch, sec_epoch):
    client.load('variables')

    body = 'Test body string.'

    resp = client.post(
        headers={
            'Host': 'localhost',
            'Content-Type': 'text/html',
            'Custom-Header': 'blah',
            'Connection': 'close',
        },
        body=body,
    )

    assert resp['status'] == 200, 'status'
    headers = resp['headers']
    header_server = headers.pop('Server')
    assert re.search(r'Unit/[\d\.]+', header_server), 'server header'
    assert (
        headers.pop('Server-Software') == header_server
    ), 'server software header'

    date = headers.pop('Date')
    assert date[-4:] == ' GMT', 'date header timezone'
    assert abs(date_to_sec_epoch(date) - sec_epoch) < 5, 'date header'

    assert headers == {
        'Connection': 'close',
        'Content-Length': str(len(body)),
        'Content-Type': 'text/html',
        'Request-Method': 'POST',
        'Request-Uri': '/',
        'Http-Host': 'localhost',
        'Server-Protocol': 'HTTP/1.1',
        'Custom-Header': 'blah',
        'Psgi-Version': '11',
        'Psgi-Url-Scheme': 'http',
        'Psgi-Multithread': '',
        'Psgi-Multiprocess': '1',
        'Psgi-Run-Once': '',
        'Psgi-Nonblocking': '',
        'Psgi-Streaming': '1',
    }, 'headers'
    assert resp['body'] == body, 'body'


def test_perl_application_query_string():
    client.load('query_string')

    resp = client.get(url='/?var1=val1&var2=val2')

    assert (
        resp['headers']['Query-String'] == 'var1=val1&var2=val2'
    ), 'Query-String header'


def test_perl_application_query_string_empty():
    client.load('query_string')

    resp = client.get(url='/?')

    assert resp['status'] == 200, 'query string empty status'
    assert resp['headers']['Query-String'] == '', 'query string empty'


def test_perl_application_query_string_absent():
    client.load('query_string')

    resp = client.get()

    assert resp['status'] == 200, 'query string absent status'
    assert resp['headers']['Query-String'] == '', 'query string absent'


@pytest.mark.skip('not yet')
def test_perl_application_server_port():
    client.load('server_port')

    assert client.get()['headers']['Server-Port'] == str(
        port_map.port(8080)
    ), 'Server-Port header'


def test_perl_application_input_read_empty():
    client.load('input_read_empty')

    assert client.get()['body'] == '', 'read empty'


def test_perl_application_input_read_parts():
    client.load('input_read_parts')

    assert (
        client.post(body='0123456789')['body'] == '0123456789'
    ), 'input read parts'


def test_perl_application_input_buffered_read():
    client.load('input_buffered_read')

    assert client.post(body='012345')['body'] == '012345', 'buffered read #1'
    assert (
        client.post(body='9876543210')['body'] == '9876543210'
    ), 'buffered read #2'


def test_perl_application_input_close():
    client.load('input_close')

    assert client.post(body='012345')['body'] == '012345', 'input close #1'
    assert (
        client.post(body='9876543210')['body'] == '9876543210'
    ), 'input close #2'


@pytest.mark.skip('not yet')
def test_perl_application_input_read_offset():
    client.load('input_read_offset')

    assert client.post(body='0123456789')['body'] == '4567', 'read offset'


def test_perl_application_input_copy():
    client.load('input_copy')

    body = '0123456789'
    assert client.post(body=body)['body'] == body, 'input copy'


def test_perl_application_input_read_after_done(findall):
    client.load('input_read_after_done')

    body = '0123456789'

    # The env holds an object whose DESTROY reads psgi.input.  It runs
    # after the request is done, when the env is released.
    resp = client.post(body=body)
    assert resp['status'] == 200, 'status'
    assert resp['headers']['X-Read'] == 'none', 'no earlier read'
    pid = resp['headers']['X-Pid']

    # This response reports the read from DESTROY.  The app then reads
    # psgi.input after $writer->close.
    resp = client.post(url='/?streaming', body=body)
    assert resp['status'] == 200, 'streaming status'
    assert resp['headers']['X-Pid'] == pid, 'same worker after DESTROY'
    assert resp['headers']['X-Read'] == '0', 'DESTROY read gets EOF'

    resp = client.get()
    assert resp['status'] == 200, 'status after streaming'
    assert resp['headers']['X-Pid'] == pid, 'same worker after close'
    assert resp['headers']['X-Read'] == '0', 'read after close gets EOF'

    assert findall(r'exited on signal') == [], 'worker crash'


def test_perl_application_errors_print(wait_for_record):
    client.load('errors_print')

    assert client.get()['body'] == '1', 'errors result'

    assert (
        wait_for_record(r'\[error\].+Error in application') is not None
    ), 'errors print'


def test_perl_application_header_equal_names():
    client.load('header_equal_names')

    assert client.get()['headers']['Set-Cookie'] == [
        'tc=one,two,three',
        'tc=four,five,six',
    ], 'header equal names'


def test_perl_application_header_pairs():
    client.load('header_pairs')

    assert client.get()['headers']['blah'] == 'blah', 'header pairs'


def test_perl_application_body_empty():
    client.load('body_empty')

    assert client.get()['body'] == '', 'body empty'


def test_perl_application_body_array():
    client.load('body_array')

    assert client.get()['body'] == '0123456789', 'body array'


def test_perl_application_body_large():
    client.load('variables')

    body = '0123456789' * 1000

    resp = client.post(body=body)['body']

    assert resp == body, 'body large'


def test_perl_application_body_io_empty():
    client.load('body_io_empty')

    assert client.get()['status'] == 200, 'body io empty'


def test_perl_application_body_io_file():
    client.load('body_io_file')

    assert client.get()['body'] == 'body\n', 'body io file'
    assert client.get()['body'] == 'body\n', 'body io file retained handle'


def test_perl_application_body_io_file_close():
    client.load('body_io_file_close')

    resp = client.get()
    assert resp['body'] == 'body\n', 'body io file'
    assert resp['headers']['X-Prev-Open'] == '0', 'no previous handle'

    # The application keeps the handle of the previous response.  The
    # module closes it when the response is sent.
    resp = client.get()
    assert resp['body'] == 'body\n', 'body io file again'
    assert resp['headers']['X-Prev-Open'] == '0', 'previous handle closed'


def test_perl_application_body_io_lines_empty():
    client.load('body_io_lines')

    resp = client.get(url='/?empty')
    assert resp['status'] == 200, 'status'
    assert resp['body'] == 'abcd', 'empty lines do not end the body'
    assert client.get()['headers']['X-Closed'] == '1', 'body closed'


def test_perl_application_body_io_lines_rs():
    client.load('body_io_lines')

    resp = client.get()
    assert resp['body'] == 'abcd', 'body'
    assert resp['headers']['X-RS'] == '0a', 'record separator'

    # The module sets $/ to read the body.  It gives the old value back.
    resp = client.get()
    assert resp['body'] == 'abcd', 'body again'
    assert resp['headers']['X-RS'] == '0a', 'record separator restored'


def test_perl_application_threads_init_fail(
    findall, skip_alert, wait_for_record
):
    skip_alert(r'PSGI: Failed to parse script')
    client.load('threads_init_fail', threads=2)

    assert client.get()['status'] == 503, 'application start fails'

    assert wait_for_record(r'second load') is not None, 'second load fails'
    assert (
        wait_for_record(r'app process \d+ exited with code 1') is not None
    ), 'application exit'

    # The second thread was never created.  The module must not join it.
    assert findall(r'join failed') == [], 'no join of a thread never created'
    assert findall(r'exited on signal') == [], 'no crash'


def test_perl_streaming_body_multiple_responses():
    client.load('streaming_body_multiple_responses')

    assert client.get()['status'] == 200


@pytest.mark.skip('not yet')
def test_perl_application_syntax_error(skip_alert):
    skip_alert(r'PSGI: Failed to parse script')
    client.load('syntax_error')

    assert client.get()['status'] == 500, 'syntax error'


def test_perl_keepalive_body():
    client.load('variables')

    assert client.get()['status'] == 200, 'init'

    body = '0123456789' * 500
    (resp, sock) = client.post(
        headers={
            'Host': 'localhost',
            'Connection': 'keep-alive',
            'Content-Type': 'text/html',
        },
        start=True,
        body=body,
        read_timeout=1,
    )

    assert resp['body'] == body, 'keep-alive 1'

    body = '0123456789'
    resp = client.post(
        headers={
            'Host': 'localhost',
            'Connection': 'close',
            'Content-Type': 'text/html',
        },
        sock=sock,
        body=body,
    )

    assert resp['body'] == body, 'keep-alive 2'


def test_perl_body_io_fake(wait_for_record):
    client.load('body_io_fake')

    assert client.get()['body'] == '21', 'body io fake'

    assert (
        wait_for_record(r'\[error\].+IOFake getline\(\) \$\/ is \d+')
        is not None
    ), 'body io fake $/ value'

    assert (
        wait_for_record(r'\[error\].+IOFake close\(\) called') is not None
    ), 'body io fake close'


def test_perl_delayed_response():
    client.load('delayed_response')

    resp = client.get()

    assert resp['status'] == 200, 'status'
    assert resp['body'] == 'Hello World!', 'body'


def test_perl_streaming_body():
    client.load('streaming_body')

    resp = client.get()

    assert resp['status'] == 200, 'status'
    assert resp['body'] == 'Hello World!', 'body'


def test_perl_application_threads():
    client.load('threads')

    assert 'success' in client.conf(
        '4', 'applications/threads/threads'
    ), 'configure 4 threads'

    socks = []

    for _ in range(4):
        sock = client.get(
            headers={
                'Host': 'localhost',
                'X-Delay': '2',
                'Connection': 'close',
            },
            no_recv=True,
        )

        socks.append(sock)

    threads = set()

    for sock in socks:
        resp = client.recvall(sock).decode('utf-8')

        client.log_in(resp)

        resp = client._resp_to_dict(resp)

        assert resp['status'] == 200, 'status'

        threads.add(resp['headers']['X-Thread'])

        assert resp['headers']['Psgi-Multithread'] == '1', 'multithread'

        sock.close()

    assert len(socks) == len(threads), 'threads differs'


def test_perl_script_cstring_nul():
    # perl "script" is consumed as a NUL-terminated C-string path; an
    # embedded NUL (survives JSON parsing) or an empty value must be
    # rejected at validation.  "spare": 0 exercises validation without
    # spawning a process.
    def conf_script(value):
        return client.conf(
            {
                "app": {
                    "type": client.get_application_type(),
                    "processes": {"spare": 0},
                    "script": value,
                }
            },
            'applications',
        )

    resp = conf_script("/x\0y.pl")
    assert 'null character' in resp.get('detail', ''), 'script nul'

    resp = conf_script("")
    assert 'must not be empty' in resp.get('detail', ''), 'script empty'
    assert 'success' in conf_script(
        f'{option.test_dir}/perl/variables/psgi.pl'
    ), 'script valid'
