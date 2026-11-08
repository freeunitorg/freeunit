import os
import sys


def application(environ, start_response):
    start_response(
        '200',
        [
            ('Content-Length', '0'),
            ('X-Executable', sys.executable),
            ('X-Executable-Ok', str(os.access(sys.executable, os.X_OK))),
        ],
    )
    return []
