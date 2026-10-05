import email


def application(environ, start_response):
    content_type = environ.get('CONTENT_TYPE', '')
    content_length = int(environ.get('CONTENT_LENGTH', 0))
    body = environ['wsgi.input'].read(content_length)

    # Construct a MIME message from the request headers + body so we can
    # use the email module to parse multipart/form-data (the cgi module
    # was removed in Python 3.13).
    msg_data = f"Content-Type: {content_type}\r\n\r\n".encode() + body
    try:
        msg = email.message_from_bytes(msg_data)
    except Exception:
        start_response('400 Bad Request', [('Content-Type', 'text/plain')])
        return [b'malformed multipart data']

    filename = ""
    file_data = b""

    # Walk all MIME parts to find the first form-data field named "file".
    # msg.walk() yields the message itself first, then each sub-part.
    for part in msg.walk():
        if part.get_content_disposition() == 'form-data':
            name = part.get_param('name', header='content-disposition')
            if name == 'file':
                filename = part.get_filename() or ""
                file_data = part.get_payload(decode=True) or b""
                break

    data = filename.encode() + file_data

    start_response(
        '200 OK',
        [('Content-Type', 'text/plain'), ('Content-Length', str(len(data)))],
    )

    return [data]
