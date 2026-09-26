def application(environ, start_response):
    traceparent = environ.get('HTTP_TRACEPARENT', '')
    start_response(
        '200',
        [('Content-Length', '0'), ('X-Seen-Traceparent', traceparent)],
    )
    return []
