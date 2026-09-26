import re


def check_http2(output_version):
    return re.search('--h2', output_version) is not None
