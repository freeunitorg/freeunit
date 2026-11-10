import os

from unit.applications.lang.python import ApplicationPython
from unit.option import option
from unit.utils import public_dir

prerequisites = {'modules': {'python': 'any'}}

client = ApplicationPython()


def test_settings_body_temp_path():
    # The router keeps "body_temp_path" after the configuration is applied
    # and the memory of the parsed configuration is released.  A --debug
    # build fills released memory with 0x5A, so a path that still points
    # there makes mkstemp() fail with "ZZZZ.../req-XXXXXXXX".

    path = f'{option.temp_dir}/body_temp'
    os.mkdir(path)
    # The router can run as another user than the tests.
    public_dir(path)

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "applications/mirror"}},
            "applications": {
                "mirror": {
                    "type": client.get_application_type(),
                    "processes": {"spare": 0},
                    "path": f'{option.test_dir}/python/mirror',
                    "working_directory": f'{option.test_dir}/python/mirror',
                    "module": "wsgi",
                }
            },
            "settings": {
                "http": {
                    "body_buffer_size": 1024,
                    "body_temp_path": path,
                }
            },
        }
    )

    body = 'b' * 8192

    for i in range(3):
        resp = client.post(body=body)
        assert resp['status'] == 200, f'status {i}'
        assert resp['body'] == body, f'body {i}'
