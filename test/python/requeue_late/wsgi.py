import os
import time

# A process started while the marker file exists sleeps at import, before
# PROCESS_READY, so its start can expire and leave it unaccounted.
_marker = os.environ.get('UNIT_LATE_MARKER')

if _marker and os.path.exists(_marker):
    time.sleep(float(os.environ.get('UNIT_LATE_DELAY', '4')))


def application(environ, start_response):
    tag = environ.get('HTTP_X_TAG', '')
    log = environ.get('HTTP_X_LOG')

    if log:
        with open(log, 'a') as f:
            f.write(tag + '\n')

    time.sleep(float(environ.get('HTTP_X_DELAY', 0)))

    content_length = int(environ.get('CONTENT_LENGTH') or 0)
    environ['wsgi.input'].read(content_length)

    body = tag.encode()

    start_response('200', [('Content-Length', str(len(body)))])
    return [body]
