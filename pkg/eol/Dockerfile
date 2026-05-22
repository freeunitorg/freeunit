FROM rust:slim-trixie

LABEL org.opencontainers.image.title="FreeUnit (eol-check)"
LABEL org.opencontainers.image.vendor="FreeUnit Community <team@freeunit.org>"

RUN set -eux; \
    export DEBIAN_FRONTEND=noninteractive; \
    apt-get -qq update; \
    apt-get -qq install --yes --no-install-recommends --no-install-suggests \
        curl ca-certificates; \
    rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

COPY entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/entrypoint.sh

WORKDIR /repo

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]