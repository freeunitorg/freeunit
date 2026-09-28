# Copyright (C) FreeUnit contributors.
"""Find raw port literals that bypass the port map (see unit/port.py).

The map is the identity at the default base, so a test that connects to a
literal ``8080``, or starts a helper on a literal ``7994``, passes every
default-base run and fails only with ``--port``.  This module reads the test
sources instead, so that such a literal fails on every run.

A number from :data:`unit.port.LITERALS` is reported unless it is:

* an argument of ``port_map.port()`` or ``port_map.expected()``;
* the ``port=`` keyword of a call, such as ``client.get(port=8081)``, which
  :meth:`unit.http.HTTP1.http` maps, or ``connect(port=8081)`` to a helper
  in the test that maps it;
* the default of a parameter named ``port``, which reaches one of the two
  forms above.

The rules see a literal, not a variable: a helper that receives ``port`` is
read on its own and must map it in one of these ways.

A string literal is data: ``"*:8080"`` in a config goes through
:class:`unit.control.Control`, and a ``Host`` header is sent verbatim.  Two
string forms are still reported when they are compared against a response,
because Unit or an application computed the other side from the real
listener: exactly a literal (``'8080'``, a ``Server-Port`` expectation) and
``localhost:8080`` or ``127.0.0.1:8080`` (a ``Location`` expectation).

The reverse mistake is reported too.  ``port_map.port(N)`` with a number that
is not in the map passes ``N`` through unchanged, so a helper port outside the
registered block escapes the map silently; and an address tuple
``('127.0.0.1', N)`` or a module constant ``X_PORT = N`` with such a number is
a port the map does not know.

Usage: ``python3 -m unit.port_lint [path ...]``, from ``test/``.
"""

import ast
import re
import sys
from pathlib import Path

from unit.port import LITERALS

_MAP_CALLS = {'port', 'expected'}

_ADDRESS = re.compile(r'(?:localhost|127\.0\.0\.1):([0-9]+)')


def _is_map_call(node):
    return isinstance(node, ast.Call) and (
        isinstance(node.func, ast.Attribute) and node.func.attr in _MAP_CALLS
    )


def _parents(tree):
    for parent in ast.walk(tree):
        for child in ast.iter_child_nodes(parent):
            child.parent = parent


def _context(node, *through):
    """The nearest ancestor that is not one of the ``through`` node types."""
    parent = node.parent

    while isinstance(parent, through):
        parent = parent.parent

    return parent


def _is_default_of_port(node, arguments):
    args = arguments.posonlyargs + arguments.args
    defaults = arguments.defaults

    pairs = list(zip(args[len(args) - len(defaults) :], defaults))
    pairs += zip(arguments.kwonlyargs, arguments.kw_defaults)

    return any(default is node and arg.arg == 'port' for arg, default in pairs)


def _is_address_tuple(node, parent):
    return (
        isinstance(parent, ast.Tuple)
        and len(parent.elts) == 2
        and parent.elts[1] is node
        and isinstance(parent.elts[0], ast.Constant)
        and isinstance(parent.elts[0].value, str)
    )


def _is_port_constant(parent):
    return (
        isinstance(parent, ast.Assign)
        and isinstance(parent.parent, ast.Module)
        and all(
            isinstance(target, ast.Name) and target.id.endswith('PORT')
            for target in parent.targets
        )
    )


def _check_int(node):
    value = node.value
    # port=8081 if sock_type == 'ipv4' else 8082
    parent = _context(node, ast.IfExp)

    if _is_map_call(parent):
        if value not in LITERALS:
            yield (
                f'port_map.{parent.func.attr}({value}) is not in the map; '
                'add the port to LITERALS in unit/port.py'
            )

        return

    if value in LITERALS:
        if (isinstance(parent, ast.keyword) and parent.arg == 'port') or (
            isinstance(parent, ast.arguments)
            and _is_default_of_port(node, parent)
        ):
            return

        yield (
            f'raw port literal {value} bypasses the port map; use '
            f'port_map.port({value})'
        )

    elif 0 < value < 65536 and (
        _is_address_tuple(node, parent) or _is_port_constant(parent)
    ):
        yield (
            f'port {value} is not in the map; add it to LITERALS in '
            f'unit/port.py and use port_map.port({value})'
        )


def _check_str(node):
    value = node.value

    if not isinstance(_context(node, ast.JoinedStr), ast.Compare):
        return

    match = _ADDRESS.search(value)
    digits = match.group(1) if match else value

    if digits.isdigit() and int(digits) in LITERALS:
        yield (
            f'raw port literal {value!r} bypasses the port map; use '
            f'port_map.port({int(digits)}) or port_map.expected()'
        )


def check(path):
    """Yield ``(line, message)`` for every finding in one source file."""
    tree = ast.parse(Path(path).read_text(), str(path))
    _parents(tree)

    for node in ast.walk(tree):
        if not isinstance(node, ast.Constant) or isinstance(node.value, bool):
            continue

        if isinstance(node.value, int):
            findings = _check_int(node)

        elif isinstance(node.value, str):
            findings = _check_str(node)

        else:
            continue

        for message in findings:
            yield node.lineno, message


def _sources(root):
    """The suite's own sources under a test directory.

    The fixture applications (python/, go/, ...) run inside Unit and some are
    deliberately broken, so only the tests and the harness are read.
    """
    yield from root.glob('test_*.py')
    yield from root.glob('conftest.py')
    yield from (root / 'unit').rglob('*.py')


def find(paths):
    """Yield ``(path, line, message)`` for every finding under ``paths``."""
    for root in paths:
        root = Path(root)
        files = sorted(_sources(root)) if root.is_dir() else [root]

        for path in files:
            if path.name in ('port.py', 'port_lint.py', 'test_port_map.py'):
                continue

            for line, message in sorted(check(path)):
                yield path, line, message


def main(argv):
    findings = list(find(argv or ['.']))

    for path, line, message in findings:
        print(f'{path}:{line}: {message}')

    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
