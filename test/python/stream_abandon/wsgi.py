import time


def application(environ, start_response):
    # Stream one chunk every 0.2 s for X-Duration seconds, then create the
    # file named by X-Marker: the worker ran the request to its end.
    duration = float(environ.get('HTTP_X_DURATION', 3))
    marker = environ.get('HTTP_X_MARKER')

    write = start_response('200', [('Content-Type', 'text/plain')])

    end = time.monotonic() + duration
    while time.monotonic() < end:
        write(b'x' * 1024)
        time.sleep(0.2)

    if marker:
        open(marker, 'w', encoding='utf-8').close()

    return []
