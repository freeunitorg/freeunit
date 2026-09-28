import json
import os

try:
    # Python 3
    from urllib.parse import parse_qs
except ImportError:
    # Python 2
    from urlparse import parse_qs


def application(environ, start_response):
    ret = {
        'FileExists': False,
    }

    d = parse_qs(environ['QUERY_STRING'])

    if 'path' in d:
        ret['FileExists'] = os.path.exists(d['path'][0])

    if 'read' in d:
        try:
            with open(d['read'][0], 'r') as f:
                ret['FileContent'] = f.read()

        except IOError:
            ret['FileContent'] = None

    out = json.dumps(ret)

    start_response(
        '200',
        [
            ('Content-Type', 'application/json'),
            ('Content-Length', str(len(out))),
        ],
    )

    return out.encode('utf-8')
