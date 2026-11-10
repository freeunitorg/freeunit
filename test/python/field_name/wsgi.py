def application(environ, start_response):
    body = b'0123456789'

    start_response(
        '200 OK',
        [
            ('Content-Type', 'text/plain'),
            ('X-Fnup', '3'),
        ],
    )
    return [body]
