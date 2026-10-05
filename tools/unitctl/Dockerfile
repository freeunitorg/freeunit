FROM rust:slim-trixie

ADD https://freeunit.org/media/keys/6C68B7AA.asc \
    /usr/share/keyrings/freeunit-keyring.gpg

RUN set -eux; \
    export DEBIAN_FRONTEND=noninteractive; \
    echo '5ac2cc2c7e1db042dfc173022de4f5e71114e2e5c7ed7af816b24cbfb687f618  /usr/share/keyrings/freeunit-keyring.gpg' | \
    sha256sum --check -; \
    chmod 0644 /usr/share/keyrings/freeunit-keyring.gpg; \
    apt-get -qq update; \
    apt-get -qq upgrade --yes; \
    apt-get -qq install --yes --no-install-recommends --no-install-suggests \
        bsdmainutils \
        ca-certificates \
        git \
        gzip \
        grep \
        gawk \
        sed \
        make \
        rpm \
        pkg-config \
        libssl-dev \
        dpkg-dev \
        musl-dev \
        musl-tools \
        gcc-aarch64-linux-gnu \
        libc6-dev-arm64-cross \
        gcc-x86-64-linux-gnu \
        libc6-dev-amd64-cross; \
    rustup target install x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu x86_64-unknown-linux-musl; \
    cargo install --quiet cargo-deb cargo-generate-rpm; \
    rm -rf /var/lib/apt/lists/* /var/tmp/* /tmp/*; \
    git config --global --add safe.directory /project