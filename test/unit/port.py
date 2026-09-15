"""Map the suite's historical port literals onto a session-wide base port.

Tests have always named their listeners literally -- ``"*:8080"``,
``"127.0.0.1:8081"``, the 7978-7999 helper registry.  That single fixed band is
why only one pytest process can run per network namespace: the capability
probes in :mod:`unit.check.chroot` and :mod:`unit.check.isolation` bind it
during session startup, before any test is selected.

The fix is not to edit ~235 literals (``tools/unit-build`` does that with
sed, on a copy, and it has the scars to show for it).  It is to translate the
port where
it is *resolved*, which happens in exactly two places: the client
(:meth:`unit.http.HTTP1.http`) and the config API
(:class:`unit.control.Control`).  ``--port N`` then moves the whole suite into a
private band and two runs can coexist.

Mapping rules
-------------

The map is a fixed offset per literal, never an independent value, so every
relative identity a test depends on survives: a second listener stays one port
above the first, a proxy target that names another listener still names it, a
``destination`` rule that must *not* match still does not, and two listeners
that collide today still collide.

===========  ==========================
literal      maps to
===========  ==========================
8080-8085    base .. base+5
8090         base+10
8443         base+363
7978-7999    base-102 .. base-81
===========  ==========================

The offsets are chosen so that **at the default base of 8080 the map is the
identity** -- every literal keeps its historical value, which is what makes the
change a no-op for CI and for anyone running without ``--port`` -- and so the
three bands cannot overlap each other for any base in range.  ``8443`` sits 363
above the base rather than next to ``8090`` for exactly that reason: it is an
*upstream* port (test/fake_upstream serving a TLS relay), not a listener, so it
belongs near the helper registry rather than in the listener block.

``65536`` is deliberately absent: ``test_routing.py`` uses it to prove Unit
rejects an out-of-range port, and mapping it would quietly turn that test into
something else.

The base is a module global, and that is a requirement rather than style.  A
test module that computes ``UPSTREAM_PORT = port(7978)`` at import time is
re-imported in the child :func:`conftest.run_process` spawns; Python 3.14's
default multiprocessing start method is ``forkserver``, so that child never ran
``pytest_configure``.  Reading the base from :data:`unit.option.option` there
raises ``AttributeError`` and the child dies with a bare "Can't connect" 5 s
later.  A module global imports cleanly, because the child is handed the
already-mapped value as a pickled argument.
"""

import re

# Linux's default net.ipv4.ip_local_port_range starts here.  The value is not
# read from /proc on purpose: the bound has to be the same on every machine that
# runs the suite, or a --port accepted on one box is rejected on the next.  A
# box with a lowered floor is the operator's problem, not the map's.
_EPHEMERAL_FIRST = 32768

# The lowest output is the helper block's first entry, at base-102, so the
# outputs stay unprivileged for any base at or above 1126.  MIN_BASE is set far
# above that floor on purpose: it keeps the whole band near the range the suite
# has always used, which is what makes a mapped run readable next to an
# unmapped one.
#
# MAX_BASE is derived from the *highest* offset rather than written down, so it
# cannot drift when an offset is added: the top output is 8443's base+363, and
# it has to stay below the ephemeral range.  A listener inside that range binds
# fine most of the time and then, once per suite, loses to an earlier client
# connection still sitting in TIME_WAIT on the same number.
MIN_BASE = 7876

# literal -> offset from the base
_BAND = {
    8080: 0,
    8081: 1,
    8082: 2,
    8083: 3,
    8084: 4,
    8085: 5,
    8090: 10,
    8443: 363,
}

# Defined here rather than next to MIN_BASE because it reads _BAND.  The helper
# block's offset is negative, so the listener band alone sets the top.
MAX_BASE = _EPHEMERAL_FIRST - 1 - max(_BAND.values())

# The helper-process registry (test/fake_upstream, test/fake_otlp), kept as its
# own block so its literals cannot collide with the listener band.  These are
# registered in test/fake_upstream/README.md and hardcoded in the Rust helpers'
# tests, so the offset has to be preserved rather than reinvented.
_BLOCK_FIRST = 7978
_BLOCK_LAST = 7999
_BLOCK_OFFSET = _BLOCK_FIRST - 8080

_BASE = 8080

# Values the current base already produces; see remap().  Empty at the identity
# base, which is what keeps a default run byte-for-byte unchanged.
_MAPPED_OUTPUTS = frozenset()

# Every number the map knows.  A run of digits is rewritten only when it *is*
# one of these.  A blanket four-digit substitution is not usable on this suite:
# "\u0000", "\x00", "bytes=000-004", "%00", a base64-ish charset string,
# HTTP-date strings and "%08d" format specs all contain four digits, and a first
# cut of this module rewrote every one of them (caught by test_return.py's
# location charset and test_static.py's byte ranges).
_LITERALS = frozenset(_BAND) | frozenset(range(_BLOCK_FIRST, _BLOCK_LAST + 1))

_NUMBER = re.compile(r'[0-9]+')


def set_base(base):
    """Set the session base port.  Called once, from ``pytest_configure``."""
    base = int(base)

    if not MIN_BASE <= base <= MAX_BASE:
        raise ValueError(
            f'--port must be between {MIN_BASE} and {MAX_BASE}, got {base}'
        )

    # A base is usable only if the map is injective and its outputs avoid the
    # literals the suite also names directly.  Checked from the literals, not by
    # intersecting the outputs of two bases: base 8080 *is* the identity, and an
    # output-set comparison wrongly rejects it.
    entries = _mapped_entries(base)

    values = [value for _, value in entries]

    if len(set(values)) != len(values):
        raise ValueError(f'--port {base} maps two literals onto one port')

    for literal, value in entries:
        if value != literal and value in _LITERALS:
            raise ValueError(
                f'--port {base} maps {literal} onto {value}, which the suite '
                'also names literally; pick another base'
            )

    global _BASE, _MAPPED_OUTPUTS
    _BASE = base
    _MAPPED_OUTPUTS = frozenset(
        str(value) for literal, value in entries if value != literal
    )


def base():
    """The session base port, for tests that need to speak in absolute terms."""
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

    if original in _BAND:
        return _BASE + _BAND[original]

    if _BLOCK_FIRST <= original <= _BLOCK_LAST:
        return _BASE + _BLOCK_OFFSET + (original - _BLOCK_FIRST)

    return original


def remap(text):
    """Map every known port literal in a string (config body or URL path).
    Only a run of digits that is *exactly* a known literal is rewritten, and
    only when it is not already the base's own value.  Every literal is four
    digits, so a longer run is left alone whatever it parses as: a zero-padded
    ``"08080"`` is not the port 8080 and must not become one.  That second guard
    matters because remap() runs on composed text: an f-string that already
    interpolated the base, or a config a previous remap() pass produced, would
    otherwise be translated a second time.

    ``client.conf()`` also accepts bytes -- test_fake_upstream's TLS case passes
    a bytes body -- so the type is preserved rather than assumed.  Port literals
    are ASCII, and a body that is not decodable has no port in it worth mapping,
    so an undecodable body is returned untouched.
    """

    def replace(match):
        run = match.group()

        if run in _MAPPED_OUTPUTS:
            return run

        if len(run) != 4 or int(run) not in _LITERALS:
            return run

        return str(port(run))

    if isinstance(text, (bytes, bytearray)):
        try:
            return type(text)(
                _NUMBER.sub(replace, text.decode('ascii')).encode('ascii')
            )

        except UnicodeDecodeError:
            return text

    return _NUMBER.sub(replace, text)


def expected(value):
    """Return ``value`` with its port literals mapped, for config comparisons.

    ``client.conf_get()`` round-trips through the map, so a test that compares a
    returned config against a literal dict has to map the literal too or it
    compares ``base+k`` against 8080+k.  Recursive because the literal can be a
    nested config, and string leaves are remapped so that
    ``"upstreams/one/servers/127.0.0.1:8081"``-style keys compare equal.
    """
    if isinstance(value, dict):
        return {expected(key): expected(item) for key, item in value.items()}

    if isinstance(value, list):
        return [expected(item) for item in value]

    if isinstance(value, str):
        return remap(value)

    return value


def _mapped_entries(base):
    """(literal, mapped value) for every literal the map knows."""
    return [
        (literal, base + offset) for literal, offset in _BAND.items()
    ] + [
        (literal, base + _BLOCK_OFFSET + (literal - _BLOCK_FIRST))
        for literal in range(_BLOCK_FIRST, _BLOCK_LAST + 1)
    ]
