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

A base is refused when a shifted literal lands on another literal (a test that
names both would see them collide, and a second pass over a mapped config
would map it again), and when the highest output, 8443's, would reach the
ephemeral range.  ``MIN_BASE`` keeps the band near the range the suite has
always used; the outputs are unprivileged for any base at or above 1128.

The base is a module global, not read from :data:`unit.option.option`: a test
module that computes ``UPSTREAM_PORT = port(7978)`` at import time is
re-imported in the child :func:`conftest.run_process` spawns, and with Python
3.14's default ``forkserver`` start method that child never ran
``pytest_configure``.
"""

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
MIN_BASE = 7876
MAX_BASE = 32767 - (max(LITERALS) - 8080)

_BASE = 8080

# A port in a configuration or a config URL follows a colon: "*:8080",
# "127.0.0.1:8081", "[::1]:8082", "http://127.0.0.1:8081"; in a range
# "*:8080-8090" the second number follows the first.  Only such a field is
# rewritten, so a literal in other data -- an environment value such as
# {"PORT": "8080"}, a URI, a "return" text or an argument -- reaches Unit
# unchanged.  The lookahead keeps an IPv6 hextet ("[2001:8080::1]") out.
# A URL in a value ({"ENDPOINT": "http://svc:8080"}) is still rewritten: no
# test sends one, and a syntax rule cannot tell it from a proxy target.
_FIELD = r'(?<=:)[0-9]+(?:-[0-9]+)?(?![0-9A-Fa-f:\]])'
_FIELD_STR = re.compile(_FIELD)
_FIELD_BYTES = re.compile(_FIELD.encode())


def set_base(base):
    """Set the session base port.  Called once, from ``pytest_configure``."""
    base = int(base)

    if not MIN_BASE <= base <= MAX_BASE:
        raise ValueError(
            f'--port must be between {MIN_BASE} and {MAX_BASE}, got {base}'
        )

    shift = base - 8080

    for literal in sorted(LITERALS):
        if shift != 0 and literal + shift in LITERALS:
            raise ValueError(
                f'--port {base} maps {literal} onto {literal + shift}, which '
                'the suite also names literally; pick another base'
            )

    global _BASE
    _BASE = base


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
    """Map every known port literal in a config body or a config URL.

    Only a port field is looked at (see ``_FIELD``), and in it only a run
    that is exactly a known literal is rewritten: a zero-padded ``"08080"``
    is not the port 8080.  ``set_base()`` guarantees that no output is itself
    a literal, so a second pass over the result changes nothing.

    ``client.conf()`` also accepts bytes, and a body with non-ASCII UTF-8 text
    still carries its ports in ASCII, so the fields are matched in the bytes
    themselves and the type is preserved.
    """

    def run(digits):
        if len(digits) == 4 and int(digits) in LITERALS:
            return str(port(digits))

        return digits

    def field(text):
        return '-'.join(run(digits) for digits in text.split('-'))

    if isinstance(text, (bytes, bytearray)):
        return type(text)(
            _FIELD_BYTES.sub(
                lambda match: field(match.group().decode()).encode(), text
            )
        )

    return _FIELD_STR.sub(lambda match: field(match.group()), text)


def expected(value):
    """Return ``value`` with its port literals mapped, for config comparisons.

    ``client.conf_get()`` round-trips through the map, so a test that compares
    a returned config against a literal has to map the literal too.  Recursive
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
