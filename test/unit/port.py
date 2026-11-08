"""Map the suite's historical port literals onto a session-wide base port.

Tests name their listeners literally -- ``"*:8080"``, ``"127.0.0.1:8081"``,
the 7976-7999 helper registry.  That single fixed band is why only one pytest
process can run per network namespace: the capability probes in
:mod:`unit.check.chroot` and :mod:`unit.check.isolation` bind it during session
startup, before any test is selected.

The port is translated where it is *resolved*, in the two places a literal
becomes a ``connect()`` or reaches unitd: the client
(:meth:`unit.http.HTTP1.http`) and the config API
(:class:`unit.control.Control`).  ``--port N`` then moves the whole suite into
a private band and two runs can coexist.  A test that opens a raw socket or
starts a helper process maps its port with :func:`port`; a test that compares
a config against a literal maps the literal with :func:`expected`.
``test/unit/port_lint.py`` finds the raw literals that bypass both.

Mapping rules
-------------

Every known literal is shifted by the same amount, ``base - 8080``, so at the
default base the map is the identity: an unmodified run is byte-for-byte
unchanged, which is what keeps CI and the container modes safe.  A constant
shift also preserves every relative identity a test depends on: a second
listener stays one port above the first, a proxy target that names another
listener still names it, a ``destination`` rule that must *not* match still
does not.

The known literals are the listener band 8080-8085 and 8090, the TLS relay
port 8443 and the helper-process registry 7976-7999 (test/fake_upstream,
registered in its README).  Only a digit run that *is* one of them is
rewritten: a blanket four-digit substitution corrupts ``\\x00``, ``%00``,
``bytes=000-004``, ``%08d`` and HTTP-date strings, all of which this suite
contains.  ``65536`` is deliberately absent: ``test_routing.py`` uses it to
prove Unit rejects an out-of-range port.

A base must be ``8080 + k * STEP``, from 8080 up to the last one whose highest
output, 8443's, stays below the ephemeral range.  The mapped band of one base
is 468 ports wide (7976 to 8443) and ``STEP`` is wider, so two different bases
never share a port: runs at 18080 and 18081 would, because the first one's
8081 is the second one's 8080.  For the same reason no output is itself a
literal, and a second pass over a mapped config changes nothing.

The base is a module global.  ``set_base()`` also writes it to the
environment, because a forkserver or spawn child of
:func:`conftest.run_process` imports this module again and does not run
``pytest_configure``: a module constant such as ``UPSTREAM_PORT =
port(7978)`` then gets the same value in the child.
"""

import json
import os
import re

# Every number the map knows.  A port() call with a literal outside this set
# passes it through unmapped, so port_lint.py refuses one, and
# test_port_map.py checks that the fake_upstream registry stays inside it.
LITERALS = frozenset(range(7976, 8000)) | frozenset(range(8080, 8086)) | {
    8090,
    8443,
}

# Linux's default net.ipv4.ip_local_port_range starts at 32768.  It is not
# read from /proc on purpose: a --port accepted on one box must be accepted on
# the next.  A listener inside that range binds fine most of the time and
# then, once per suite, loses to an earlier client connection still in
# TIME_WAIT on the same number.
STEP = 1000
assert STEP > max(LITERALS) - min(LITERALS)
MIN_BASE = 8080
MAX_BASE = MIN_BASE + STEP * ((32767 - max(LITERALS)) // STEP)

_BASE = int(os.environ.get('_UNIT_TEST_PORT_BASE', MIN_BASE))

# A port in a configuration or a config URL follows a colon: "*:8080",
# "127.0.0.1:8081", "[::1]:8082", "http://127.0.0.1:8081"; in a range
# "*:8080-8090" the second number follows the first.  Only such a field is
# rewritten, so a literal in other data -- an environment value such as
# {"PORT": "8080"}, a URI, a "return" text or an argument -- reaches Unit
# unchanged.  The lookahead keeps an IPv6 group ("[2001:8080::1]") out, and
# remap() skips a field whose host has a colon, so the last group of an
# address without brackets ("2001:db8::8080") is not a port either.
# The lookbehind refuses a colon after a quote or a white space: no address
# has one before its port colon.
# A URL in a value ({"ENDPOINT": "http://svc:8080"}) is still rewritten: no
# test sends one, and a syntax rule cannot tell it from a proxy target.
_FIELD = re.compile(r'(?<=[^"\s]:)[0-9]+(?:-[0-9]+)?(?![0-9A-Fa-f:\]])')

# A JSON string literal.  In a config body every address is a string (a key
# or a value), so remap_body() looks only inside these, after it decodes
# them: a JSON number is never a port, and an escape such as "*\u003a8080"
# is seen as the "*:8080" that Unit sees.
_JSON_STRING = re.compile(r'"[^"\\]*(?:\\.[^"\\]*)*"')


def set_base(base):
    """Set the session base port.  Called once, from ``pytest_configure``."""
    base = int(base)

    if not MIN_BASE <= base <= MAX_BASE or (base - MIN_BASE) % STEP:
        raise ValueError(
            f'--port must be one of {MIN_BASE}, {MIN_BASE + STEP}, ... '
            f'{MAX_BASE}, got {base}'
        )

    global _BASE
    _BASE = base
    os.environ['_UNIT_TEST_PORT_BASE'] = str(base)


def base():
    """The session base port, for a test that speaks in absolute terms."""
    return _BASE


def port(original):
    """Map one port literal.  Unknown values, and non-integers, pass through.

    ``HTTP1.http()`` resolves its port through here, and callers legitimately
    pass ``port=None`` for a unix-socket request (test_unix_abstract.py's
    address table), so a non-integer has to survive rather than raise.
    """
    try:
        original = int(original)

    except (TypeError, ValueError):
        return original

    if original in LITERALS:
        return original + _BASE - 8080

    return original


def remap(text):
    """Map every known port literal in a config URL or a decoded string.

    Only a port field is looked at (see ``_FIELD``), and in it only a run
    that is exactly a known literal is rewritten: a zero-padded ``"08080"``
    is not the port 8080.  ``set_base()`` guarantees that no output is itself
    a literal, so a second pass over the result changes nothing.
    """

    def run(digits):
        if len(digits) == 4 and int(digits) in LITERALS:
            return str(port(digits))

        return digits

    def field(text):
        return '-'.join(run(digits) for digits in text.split('-'))

    def sub(match):
        host = re.split(r'[\s"/@\[\]]', text[: match.start() - 1])[-1]

        return match.group() if ':' in host else field(match.group())

    return _FIELD.sub(sub, text)


def remap_body(body):
    """Map the ports in the strings of a serialized config body.

    Each JSON string is decoded, mapped with ``remap()``, and encoded again
    only if it changed; the rest of the text stays as it is.  A token that is
    not a valid JSON string is mapped as text.

    ``client.conf()`` also accepts bytes.  They are decoded with
    ``surrogateescape``, so a body that is not UTF-8 keeps its bytes, and the
    type is preserved.
    """
    if isinstance(body, (bytes, bytearray)):
        text = bytes(body).decode('utf-8', 'surrogateescape')

        return type(body)(
            remap_body(text).encode('utf-8', 'surrogateescape')
        )

    def string(match):
        token = match.group()

        try:
            value = json.loads(token)

        except ValueError:
            return remap(token)

        mapped = remap(value)

        if mapped == value:
            return token

        # Keep the escapes of the token, such as a lone "\ud800", which
        # json.dumps() cannot write back as UTF-8.
        text = remap(token)

        try:
            if json.loads(text) == mapped:
                return text

        except ValueError:
            pass

        return json.dumps(mapped, ensure_ascii=False)

    return _JSON_STRING.sub(string, body)


def expected(value):
    """Return ``value`` with its port literals mapped, for config comparisons.

    ``client.conf()`` maps what it sends and ``client.conf_get()`` does not
    map the reply back, so a test that compares a returned config against a
    literal has to map the literal too.  Recursive
    because the literal can be a nested config; keys are mapped as well, so
    ``"upstreams/one/servers/127.0.0.1:8081"``-style keys compare equal.
    """
    if isinstance(value, dict):
        return {expected(key): expected(item) for key, item in value.items()}

    if isinstance(value, list):
        return [expected(item) for item in value]

    if isinstance(value, str):
        return remap(value)

    return value
