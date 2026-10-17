# SELinux policy module for unitd (proof of concept)

Status: **experimental, not packaged, not loaded by any package.**

A minimal refpolicy-style module confining `unitd` in `unitd_t`:

| Type               | Labels                                   |
|--------------------|------------------------------------------|
| `unitd_exec_t`     | `/usr/sbin/unitd`                        |
| `unitd_var_run_t`  | `/run/unit`, including `control.unit.sock` |
| `unitd_log_t`      | `/var/log/unit`                          |
| `unitd_var_lib_t`  | `/var/lib/unit` (state directory)        |
| `unitd_unit_file_t`| systemd unit files                       |

Listeners may bind `http_port_t` (80, 443, 8080, ...). Other ports need
`semanage port -a -t http_port_t -p tcp <port>`.

Known gaps: application processes run in `unitd_t`; no domain transition
per language module; no booleans; isolation (namespaces, cgroups) and
rootfs are not covered. Rules must be refined from AVC denials in
permissive mode (`audit2allow`).

## Build (Fedora/RHEL, needs selinux-policy-devel)

    make -f /usr/share/selinux/devel/Makefile unitd.pp

## Static checks

    selint -s -r unitd.te unitd.if unitd.fc   # SELint
    sedismod unitd.pp                           # inspect the module (checkpolicy)

`seinfo`/`sesearch` (setools) need a binary policy, e.g. the loaded
policy after `semodule -i` on a test VM: `sesearch -A -s unitd_t`.

## Test (on a disposable VM only)

    semodule -i unitd.pp
    restorecon -Rv /usr/sbin/unitd /run/unit /var/log/unit /var/lib/unit
    semanage permissive -a unitd_t
    systemctl restart unit
    ausearch -m AVC -c unitd | audit2allow -R

Remove with `semodule -r unitd`.
