import json
import os
import time
from urllib.parse import parse_qs


def record(event, environ, **extra):
    rec = {
        'event': event,
        'time': time.time(),
        'pid': os.getpid(),
        'uri': environ.get('REQUEST_URI'),
        'path': environ.get('PATH_INFO'),
        'query': environ.get('QUERY_STRING'),
        'host': environ.get('HTTP_HOST'),
        'user_agent': environ.get('HTTP_USER_AGENT'),
        'x_cron': environ.get('HTTP_X_CRON'),
        'server_name': environ.get('SERVER_NAME'),
        'server_port': environ.get('SERVER_PORT'),
        'remote_addr': environ.get('REMOTE_ADDR'),
        'method': environ.get('REQUEST_METHOD'),
        'fields': {k: v for k, v in environ.items() if k.startswith('HTTP_')},
    }
    rec.update(extra)

    # One write() on an O_APPEND descriptor: the records of two processes
    # do not mix, and a reader sees a record end only once it is complete.
    data = (json.dumps(rec) + '\n').encode()
    fd = os.open(os.environ['SCHEDULE_LOG'],
                 os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)

    try:
        assert os.write(fd, data) == len(data)
    finally:
        os.close(fd)


def application(environ, start_response):
    args = parse_qs(environ.get('QUERY_STRING', ''))

    record('start', environ)

    # ?hang=<path>: the first request creates <path> and never answers.
    hang = args.get('hang', [None])[0]

    if hang is not None and not os.path.exists(hang):
        open(hang, 'w').close()
        time.sleep(3600)

    time.sleep(float(args.get('sleep', ['0'])[0]))

    status = args.get('status', ['200'])[0]
    body = f'ran {environ.get("REQUEST_URI")}\n'.encode()
    stream = float(args.get('stream', ['0'])[0])

    if stream == 0:
        record('end', environ, status=int(status))

    start_response(
        f'{status} Schedule',
        [('Content-Type', 'text/plain'), ('Content-Length', str(len(body)))],
    )

    if stream == 0:
        return [body]

    # Send the response header and a part of the body, then wait.
    def body_parts():
        yield body[:1]
        time.sleep(stream)
        record('end', environ, status=int(status))
        yield body[1:]

    return body_parts()
