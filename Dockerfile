# Multi-platform image to make it easy to copy the binary into another Dockerfile.
# https://github.com/openSUSE/catatonit#usage

FROM alpine:3 AS build

RUN apk add --no-cache \
        build-base \
        autoconf \
        automake \
        libtool

WORKDIR /dist

COPY autogen.sh catatonit.c config.h.in configure.ac Makefile.am ./

RUN ./autogen.sh
RUN ./configure LDFLAGS="-static" --prefix=/ --bindir=/
RUN make


FROM scratch AS runtime

# Store without write permissions for nobody (Linux standard user for servers)
COPY --from=build --chmod=555 --chown=65534 /dist/catatonit /

# COPY --from=opensuse/catatonit --chown=65534 /catatonit /
ENTRYPOINT ["/catatonit", "--"]
# CMD ["/actual/binary", "or", "script", "--args"]
