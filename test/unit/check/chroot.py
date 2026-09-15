import json

from unit import port as port_map
from unit.http import HTTP1
from unit.option import option

http = HTTP1()


def check_chroot():
    # This probe PUTs its config through http.put() directly rather than
    # through Control, so it bypasses the config-side port map and names
    # the base itself.
    # The read is inside the function on purpose: conftest imports
    # discover_available (and therefore this module) at import time, before
    # pytest_configure has set the base.
    return (
        'success'
        in http.put(
            url='/config',
            sock_type='unix',
            addr=f'{option.temp_dir}/control.unit.sock',
            body=json.dumps(
                {
                    "listeners": {f"*:{port_map.base()}": {"pass": "routes"}},
                    "routes": [
                        {
                            "action": {
                                "share": option.temp_dir,
                                "chroot": option.temp_dir,
                            }
                        }
                    ],
                }
            ),
        )['body']
    )
