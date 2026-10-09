import json

from unit import port as port_map
from unit.http import HTTP1
from unit.option import option

http = HTTP1()


def check_chroot():
    conf = {
        "listeners": {"*:8080": {"pass": "routes"}},
        "routes": [
            {
                "action": {
                    "share": option.temp_dir,
                    "chroot": option.temp_dir,
                }
            }
        ],
    }

    return (
        'success'
        in http.put(
            url='/config',
            sock_type='unix',
            addr=f'{option.temp_dir}/control.unit.sock',
            # Direct http.put() bypasses Control, so the listener literal
            # is mapped here.
            body=port_map.remap_body(json.dumps(conf)),
        )['body']
    )
