import os

SEGMENT = 10 * 1024 * 1024


def own_segments():
    """Shared memory segments that this process created for its responses:
    mappings named unit.<pid>. (nxt_unit_new_mmap() in src/nxt_unit.c), and
    at least one segment long.  All of them can have the same name, so the
    mappings are counted, not the names.  Read here, because the test
    process may not be allowed to read /proc/<pid>/maps of another user."""

    name = f'unit.{os.getpid()}.'
    segments = 0

    with open('/proc/self/maps') as maps:
        for line in maps:
            parts = line.split(None, 5)

            if len(parts) < 6 or name not in parts[5]:
                continue

            start, _, end = parts[0].partition('-')

            if int(end, 16) - int(start, 16) >= SEGMENT:
                segments += 1

    return segments


def application(env, start_response):
    length = env.get('HTTP_X_LENGTH', '10')

    start_response(
        '200',
        [('Content-Length', length), ('X-Segments', str(own_segments()))],
    )

    return [b'x' * int(length)]
