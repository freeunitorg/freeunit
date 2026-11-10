import atexit


class application:
    def __init__(self, environ, start_response):
        self.environ = environ
        self.start = start_response

    def __iter__(self):
        atexit.register(self._atexit)

        content_length = int(self.environ.get('CONTENT_LENGTH', 0))
        body = bytes(self.environ['wsgi.input'].read(content_length))

        self.write = self.start(
            '200',
            [
                ('Content-Length', str(len(body))),
            ],
        )
        yield body

    def _atexit(self):
        for fn in (
            lambda: self.environ['wsgi.input'].read(1),
            lambda: self.environ['wsgi.input'].readline(),
            lambda: self.environ['wsgi.input'].readlines(),
            lambda: next(self.environ['wsgi.input']),
            lambda: self.write(b''),
            lambda: self.write(b'x'),
        ):
            try:
                fn()
            except RuntimeError:
                continue

            return

        self.start('200', [('Content-Length', '0')])
