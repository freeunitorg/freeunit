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

    # X-Body replaces the generated part, and X-Content-Length the length
    # the response declares, so a test can send fewer bytes than declared.
    custom = environ.get('HTTP_X_BODY')
    if custom is not None:
        part = custom.encode()
        size = len(part)

    clen = environ.get('HTTP_X_CONTENT_LENGTH', str(size * parts))

    # X-Content-Length: none sends no Content-Length, so the router does not
    # know the length and sends the body chunked.
    headers = [('Content-Type', 'text/plain')]
    if clen != 'none':
        headers.append(('Content-Length', clen))

    # A large field makes the response buffer too large for a plain port
    # message (1024 bytes), so libunit sends it through shared memory.
    pad = int(environ.get('HTTP_X_PAD', '0'))
    if pad:
        headers.append(('X-Pad', 'p' * pad))

    start_response('200 OK', headers)

    def body():
        for i in range(parts):
            if i:
                time.sleep(delay)

            yield part

    return body()
