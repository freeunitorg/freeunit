def application(environ, start_response):
    # Echo the request body, labelled as gzip: the application says that it
    # coded the body itself.
    content_length = int(environ.get('CONTENT_LENGTH', 0))
    body = bytes(environ['wsgi.input'].read(content_length))

    start_response(
        '200',
        [
            ('Content-Type', 'text/plain'),
            ('Content-Length', str(len(body))),
            ('Content-Encoding', 'gzip'),
        ],
    )
    return [body]
