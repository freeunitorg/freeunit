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

    with open(os.environ['SCHEDULE_LOG'], 'a', encoding='utf-8') as f:
        f.write(json.dumps(rec) + '\n')


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
