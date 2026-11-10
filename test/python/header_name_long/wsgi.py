def application(environ, start_response):
    name = 'X' * int(environ.get('HTTP_X_LENGTH', '300'))

    start_response('200', [('Content-Length', '4'), (name, 'value')])
    return [b'body']
