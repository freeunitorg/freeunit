"""The traceparent an application sees; test_otel_traceparent_app_php.py
runs the same checks against PHP."""

import re
import time

import pytest

from test_otel import PARENT_ID, TRACE_ID, _get_free_port
from unit.applications.lang.python import ApplicationPython

prerequisites = {'modules': {'python': 'all'}}

client = ApplicationPython()

INBOUND = f'00-{TRACE_ID}-{PARENT_ID}-01'
VALID = re.compile(r'^[0-9a-f]{2}-[0-9a-f]{32}-[0-9a-f]{16}-[0-9a-f]{2}$')


def seen(app, traceparent=None, otel=True):
    app.load('traceparent')

    if otel:
        conf = app.conf_get()
        conf.setdefault('settings', {})['telemetry'] = {
            'endpoint': f'http://127.0.0.1:{_get_free_port()}/v1/traces',
            'protocol': 'http',
            'sampling_ratio': 1.0,
            'batch_size': 1,
        }
        resp = app.conf(conf)

        if 'telemetry' in str(resp).lower():
            pytest.skip('unit built without --otel')

        assert 'success' in resp, resp

    headers = {'Host': 'localhost', 'Connection': 'close'}
    if traceparent is not None:
        headers['traceparent'] = traceparent

    # otel (re)init races the first requests.
    for _ in range(150):
        resp = app.get(headers=headers)
        value = resp['headers'].get('X-Seen-Traceparent', '')

        if resp['status'] == 200 and value:
            return value

        time.sleep(0.1)

    return value


def check_forwarded(app):
    assert seen(app, INBOUND, otel=False) == INBOUND


def check_generated(app):
    value = seen(app)
    assert VALID.match(value) and TRACE_ID not in value, value


def check_inherited(app):
    # The parent-id must be FreeUnit's own span, so app spans are children.
    value = seen(app, INBOUND)
    assert VALID.match(value) and TRACE_ID in value, value
    assert PARENT_ID not in value, value


def check_malformed(app):
    value = seen(app, 'not-a-traceparent')
    assert VALID.match(value), value


def test_traceparent_forwarded_without_otel():
    check_forwarded(client)


def test_traceparent_generated_when_missing_with_otel():
    check_generated(client)


def test_traceparent_inherited_with_otel():
    check_inherited(client)


def test_traceparent_malformed_replaced_with_otel():
    check_malformed(client)
