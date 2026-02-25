FROM debian:bullseye-slim

RUN apt-get update && apt-get install -y \
    curl \
    bzip2 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Bootlin 2024 (sh4 + musl libc)
RUN curl -fsSL https://toolchains.bootlin.com/downloads/releases/toolchains/sh-sh4/tarballs/sh-sh4--musl--stable-2024.02-1.tar.bz2 | tar xj -C /opt

ENV PATH="/opt/sh-sh4--musl--stable-2024.02-1/bin:${PATH}"

WORKDIR /usr/src/app
