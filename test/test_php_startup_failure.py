import pytest

from unit.applications.lang.php import ApplicationPHP

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()

BOUNDARY = 'startup-failure'

# One text field that is bigger than "memory_limit" and smaller than both
# "post_max_size" and the router limit "max_body_size" (8 MiB by default).
FIELD_SIZE = 6 * 1024 * 1024


def post_oversized_form():
    """POST a multipart body that runs PHP out of memory during startup.

    PHP parses the body inside php_request_startup(), before the script
    runs.  The memory error is therefore a failed request startup, not a
    failed script.
    """

    body = (
        f'--{BOUNDARY}\r\n'
        'Content-Disposition: form-data; name="field"\r\n'
        '\r\n'
        f'{"a" * FIELD_SIZE}\r\n'
        f'--{BOUNDARY}--\r\n'
    )

    return client.post(
        headers={
            'Host': 'localhost',
            'Content-Type': f'multipart/form-data; boundary={BOUNDARY}',
            'Connection': 'close',
        },
        body=body,
    )


def test_php_startup_failure_worker_exits(
    skip_alert, wait_for_record, findall
):
    """A worker whose request startup failed must not serve another request.

    Without the exit, the worker served the next requests on top of the
    failed setup.  On PHP 8.5.11, two failed startups in a row left it
    answering 503 to every request.  On PHP 8.5.12, every second oversized
    request got a 200 with an empty body.  With the exit, each failure ends
    the worker and the router starts a new one.
    """

    skip_alert(r'php_request_startup\(\) failed')

    client.load(
        'startup_failure',
        options={'admin': {'memory_limit': '4M', 'post_max_size': '8M'}},
    )

    resp = client.get()
    assert resp['status'] == 200, 'application starts'

    pid, limit, version_id = resp['body'].split()
    assert limit == '4M', 'memory_limit is applied'

    statuses = [post_oversized_form()['status'] for _ in range(3)]

    if 503 not in statuses:
        # This PHP handled the memory error without failing the request
        # startup, so there is no failed startup for the worker to exit on.
        pytest.skip('this PHP does not fail the request startup')

    resp = client.get()
    assert resp['status'] == 200, 'worker serves after failed startups'
    assert resp['body'].split()[0] != pid, 'worker was replaced'

    logged = wait_for_record(
        r'\[alert\] .*php_request_startup\(\) failed, the worker exits'
    )

    hard = findall(fr'app process {pid} exited with code 137\b')

    if logged is None and hard:
        # From PHP 8.2.35 on, an allocation past the hard memory limit
        # (200 % of memory_limit by default) ends the process with
        # _exit(137) inside the startup, before the module can log.
        pytest.skip('this PHP reached its hard memory limit')

    if logged is None and int(version_id) < 80200:
        # The source backports for 7.4, 8.0 and 8.1 end the process inside the
        # startup, before the module can log.
        pytest.skip('this PHP ends the process itself on a memory error')

    assert logged is not None, 'the failed startup is logged'
    assert (
        wait_for_record(fr'app process {pid} exited with code 1\b') is not None
    ), 'the first worker exited with code 1'
