import os
import platform


class Options:
    _options = {
        'architecture': platform.architecture()[0],
        'available': {'modules': {}, 'features': {}},
        'configure_flag': {},
        'is_privileged': os.geteuid() == 0,
        'skip_alerts': [],
        # Set by a test that must ignore sanitizer reports from other
        # processes (test_http2.py's leak test); reset for each test.
        'skip_sanitizer': False,
        'system': platform.system(),
        'unsafe': False,
    }

    def __setattr__(self, name, value):
        Options._options[name] = value

    def __getattr__(self, name):
        if name in Options._options:
            return Options._options[name]

        raise AttributeError


option = Options()
