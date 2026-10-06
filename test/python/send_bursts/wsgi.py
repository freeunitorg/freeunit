import time


def application(environ, start_response):
    # Sends the body in bursts of parts.  The first burst has X-First parts,
    # each next burst has X-Parts parts.  A burst starts each X-Interval
    # seconds.  The parts of a burst are 10 ms apart, so the router gets and
    # writes each part alone.
    first = int(environ.get('HTTP_X_FIRST', 1))
    parts = int(environ.get('HTTP_X_PARTS', 1))
    size = int(environ.get('HTTP_X_PART_SIZE', 1))
    bursts = int(environ.get('HTTP_X_BURSTS', 1))
    interval = float(environ.get('HTTP_X_INTERVAL', 0))

    length = size * (first + parts * (bursts - 1))

    write = start_response('200', [('Content-Length', str(length))])

    part = b'x' * size
    start = time.monotonic()

    for burst in range(bursts):
        delay = start + burst * interval - time.monotonic()
        if delay > 0:
            time.sleep(delay)

        for _ in range(first if burst == 0 else parts):
            write(part)
            time.sleep(0.01)

    return []
