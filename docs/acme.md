# ACME certificates with an external client

ACME is the protocol that certificate authorities use to issue and renew
certificates. Let's Encrypt is one of these authorities. FreeUnit does not
connect to a certificate authority. An external ACME client gets and renews
the certificate. Two examples are certbot and lego. A deploy hook then
uploads the new bundle through the control API. A bundle is one PEM body
that holds the server certificate, its chain, and the private key.

Renewal needs no restart. `PUT /certificates/<name>` on an existing name
replaces the bundle in place. When a listener names the bundle, FreeUnit
applies the configuration again. Connections that were already accepted
finish with the old certificate. New handshakes get the new certificate.

This page gives the procedure for operators.

## What the control API does

- `PUT /certificates/<name>` creates the bundle, or replaces it when the
  name exists. The body is a PEM bundle. The first certificate must be the
  server certificate. The chain certificates and the private key follow.
- Main writes the bundle to a temporary file in the certificate store and
  renames it over the old file. A crash never leaves a partial bundle.
- The key must belong to the first certificate. The answer to a bundle
  with the wrong key is `400 Invalid certificate.`. The old bundle stays in
  use.
- A name that starts with `.` is reserved. The answer to such a name is
  `400 Invalid certificate name.`.
- The answer to a bundle over 1 MiB is `413 Certificate bundle is too
  large.`.
- When the current configuration names the bundle, the answer is
  `200 Certificate chain updated.`. The router has loaded the new bundle
  at that point. Otherwise the answer is `200 Certificate chain uploaded.`.
- `500 Failed to store certificate.` means main could not write the file.
  Nothing changed.
- `500 Certificate stored but not applied.` means the new bundle is on
  disk, but the router refused the configuration with it. One cause is a
  `conf_commands` option that does not accept the new key type. The router
  keeps the old certificate in use. `GET /certificates/<name>` already
  shows the metadata of the new bundle. The next reconfiguration or restart
  loads the new bundle. Upload a working bundle again, or correct the
  configuration.
- The request waits while another configuration change is in progress, as
  `PUT /config` does.

## HTTP-01 route on port 80

HTTP-01 is an ACME challenge type. The authority gets a token file over
plain HTTP from `http://<domain>/.well-known/acme-challenge/<token>`. The
ACME client writes the token file into a directory. FreeUnit serves that
directory with `share`. The router runs as the `--user` user. The router
must be able to read the token files. It must also be able to enter each
directory above them.

```json
{
  "listeners": {
    "*:80":  { "pass": "routes/http" },
    "*:443": { "pass": "routes/app", "tls": { "certificate": "example.org" } }
  },
  "routes": {
    "http": [
      {
        "match": { "uri": "/.well-known/acme-challenge/*" },
        "action": {
          "share": "/var/lib/unit-acme$uri",
          "chroot": "/var/lib/unit-acme/.well-known/acme-challenge/",
          "fallback": { "return": 404 }
        }
      },
      { "action": { "return": 301, "location": "https://$host$request_uri" } }
    ],
    "app": [ { "action": { "pass": "applications/app" } } ]
  }
}
```

Leave out the `*:443` listener until the first bundle exists. FreeUnit
refuses a configuration where a listener names a missing bundle.

## The deploy hook

The hook uploads the renewed bundle under the same name every time. It
needs only `curl` and write access to the control socket.

```sh
#!/bin/sh
# Deploy hook for certbot. lego uses it through the wrapper below.
#   certbot sets RENEWED_LINEAGE, for example /etc/letsencrypt/live/example.org
#   lego    sets CERT_FULLCHAIN, CERT_KEY and UNIT_CERT_NAME through the wrapper
set -eu

SOCK=${UNIT_CONTROL:-/var/run/control.freeunit.sock}
FULLCHAIN=${CERT_FULLCHAIN:-${RENEWED_LINEAGE:-}/fullchain.pem}
KEY=${CERT_KEY:-${RENEWED_LINEAGE:-}/privkey.pem}
NAME=${UNIT_CERT_NAME:-$(basename "${RENEWED_LINEAGE:?set RENEWED_LINEAGE or UNIT_CERT_NAME}")}

cat "$FULLCHAIN" "$KEY" \
    | curl -fsS --unix-socket "$SOCK" -X PUT --data-binary @- \
          "http://localhost/certificates/$NAME"
```

Install it as `/usr/local/sbin/freeunit-deploy-hook`, mode 0755.

The default socket is the socket of the FreeUnit Debian package. For a
different installation, set `UNIT_CONTROL` to its control socket:

| Installation | Control socket |
|---|---|
| FreeUnit Debian package | `/var/run/control.freeunit.sock` |
| RPM package | `/var/run/unit/control.sock` |
| Docker image | `/var/run/control.unit.sock` |
| Source build | `<runstatedir>/control.unit.sock`, or the `--control` option of `configure` |

`unitd --help` shows the default socket of the binary under `--control`.
A service can set a different socket with `--control`. Look for this
option in the unit file of the service.

`curl -f` returns an error for each answer of 400 or higher. The ACME
client then reports a failed upload.

### certbot

```sh
mkdir -p /etc/letsencrypt/renewal-hooks/deploy
ln -s /usr/local/sbin/freeunit-deploy-hook \
      /etc/letsencrypt/renewal-hooks/deploy/freeunit
certbot certonly --webroot -w /var/lib/unit-acme -d example.org -d www.example.org
RENEWED_LINEAGE=/etc/letsencrypt/live/example.org /usr/local/sbin/freeunit-deploy-hook
```

With the link, certbot runs the hook after each renewal. certbot sets
`RENEWED_LINEAGE` for the hook. The last line does the first upload. Add
the `*:443` listener after it. The `certbot.timer` unit of the distribution
runs `certbot renew` two times each day.

### lego

```sh
cat > /usr/local/sbin/lego-freeunit <<'EOF'
#!/bin/sh
CERT_FULLCHAIN=$LEGO_CERT_PATH CERT_KEY=$LEGO_CERT_KEY_PATH \
UNIT_CERT_NAME=$LEGO_CERT_DOMAIN exec /usr/local/sbin/freeunit-deploy-hook
EOF
chmod 0755 /usr/local/sbin/lego-freeunit

lego --email ops@example.org --accept-tos --path /var/lib/lego \
     --domains example.org --http --http.webroot /var/lib/unit-acme \
     run --run-hook /usr/local/sbin/lego-freeunit
```

lego sets `LEGO_CERT_PATH`, `LEGO_CERT_KEY_PATH` and `LEGO_CERT_DOMAIN` for
its hook. The wrapper maps them to the variables of the deploy hook.

For a wildcard certificate, use `--dns <provider>` instead of `--http`.
FreeUnit then needs no challenge route.

### A renewal timer for lego

lego has no timer. Use a systemd timer that runs one time each day. lego
renews a certificate only when it is close to expiry.

```ini
# /etc/systemd/system/lego-renew.service
[Unit]
Description=Renew certificates with lego and deploy them to FreeUnit

[Service]
Type=oneshot
ExecStart=/usr/bin/lego --email ops@example.org --accept-tos \
    --path /var/lib/lego --domains example.org \
    --http --http.webroot /var/lib/unit-acme \
    renew --no-random-sleep --renew-hook /usr/local/sbin/lego-freeunit
```

```ini
# /etc/systemd/system/lego-renew.timer
[Unit]
Description=Daily certificate renewal

[Timer]
OnCalendar=daily
RandomizedDelaySec=1h
Persistent=true

[Install]
WantedBy=timers.target
```

```sh
systemctl enable --now lego-renew.timer
```

To use cron instead, add this line: `17 3 * * * root /usr/bin/lego ... renew --no-random-sleep --renew-hook /usr/local/sbin/lego-freeunit`.

## Notes

- Do not manually edit the files in the state directory. The next
  reconfiguration loads a bundle that you changed there, but
  `GET /certificates` shows the old metadata until a restart.
- The hook must run as a user that can write to the control socket. Do
  not give an application process access to the socket.
