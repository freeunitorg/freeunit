"""test_otel_traceparent_app.py against $_SERVER['HTTP_TRACEPARENT']."""

from test_otel_traceparent_app import (
    check_forwarded,
    check_generated,
    check_inherited,
    check_malformed,
)
from unit.applications.lang.php import ApplicationPHP

prerequisites = {'modules': {'php': 'any'}}

client = ApplicationPHP()


def test_traceparent_forwarded_without_otel():
    check_forwarded(client)


def test_traceparent_generated_when_missing_with_otel():
    check_generated(client)


def test_traceparent_inherited_with_otel():
    check_inherited(client)


def test_traceparent_malformed_replaced_with_otel():
    check_malformed(client)
