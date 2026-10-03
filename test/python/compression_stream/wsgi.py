import time


def application(environ, start_response):
    # The body holds only the token of this request, so the bytes of another
    # response are easy to see.  It is sent in several writes with a pause
    # between them.  Each write is one message to the router, so the router
    # compresses the body in several event loop turns.
    rid = int(environ.get('HTTP_X_ID', '0'))
    parts = int(environ.get('HTTP_X_PARTS', '8'))
    size = int(environ.get('HTTP_X_SIZE', '4096'))
    delay = float(environ.get('HTTP_X_DELAY', '0.02'))

    token = b'<%06d>' % rid
    part = (token * (size // len(token) + 1))[:size]

    start_response(
        '200 OK',
        [
            ('Content-Type', 'text/plain'),
            ('Content-Length', str(size * parts)),
        ],
    )

    def body():
        for i in range(parts):
            if i:
                time.sleep(delay)

            yield part

    return body()
