def application(environ, start_response):
    start_response(
        '200',
        [
            ('Content-Length', '0'),
            ('X-Cookie', environ.get('HTTP_COOKIE', '')),
            ('X-Dup', environ.get('HTTP_X_DUP', '')),
            ('X-Content-Type', environ.get('CONTENT_TYPE', '')),
            ('X-Http-Content-Type', environ.get('HTTP_CONTENT_TYPE', '')),
        ],
    )
    return []
