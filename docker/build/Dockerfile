# Retrolities build image: Open Watcom v2 cross toolchain
# (MS-DOS, Windows 3.x, Windows 9x, Windows NT/2000 targets) + mTCP sources
FROM debian:bookworm-slim

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl xz-utils unzip zip make python3 \
      gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 \
 && rm -rf /var/lib/apt/lists/*

# Complete Open Watcom v2 snapshot (compilers, linkers, wrc, headers and
# libraries for every target).  Override OW_URL to pin a specific build.
ARG OW_URL=https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/ow-snapshot.tar.xz
RUN mkdir -p /opt/watcom \
 && curl -fsSL "$OW_URL" | tar -xJ -C /opt/watcom \
 && chmod -R a+rX /opt/watcom

# mTCP (GPLv3, M. Brutman) - TCP/IP stack available to DOS network utilities.
# Taken from the FreeDOS package mirror; file names are lower-cased so the
# sources build on a case-sensitive file system.
ARG MTCP_URL=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/latest/net/mtcp.zip
RUN mkdir -p /tmp/mtcp /opt/mtcp \
 && curl -fsSL -o /tmp/mtcp/mtcp.zip "$MTCP_URL" \
 && unzip -q /tmp/mtcp/mtcp.zip 'SOURCE/*' 'NET/mTCP/COPYING.TXT' -d /tmp/mtcp \
 && unzip -q -LL /tmp/mtcp/SOURCE/mTCP/SOURCES.ZIP 'tcpinc/*' 'tcplib/*' 'include/*' -d /opt/mtcp \
 && cp /tmp/mtcp/NET/mTCP/COPYING.TXT /opt/mtcp/COPYING.TXT \
 && sed -i -E 's/^([[:space:]]*#[[:space:]]*include[[:space:]]+")([^"]+)"/\1\L\2"/' /opt/mtcp/tcpinc/* /opt/mtcp/tcplib/* /opt/mtcp/include/* \
 && chmod -R a+rX /opt/mtcp && rm -rf /tmp/mtcp

# Patch: send a TCP window update whenever the receive window re-opens from
# less than one segment (stock mTCP only does so from exactly zero, which can
# stall peers doing sender-side silly-window avoidance on bulk transfers).
RUN grep -q 'if ( origWin == 0 ) {' /opt/mtcp/tcplib/tcp.cpp \
 && sed -i 's/if ( origWin == 0 ) {/if ( origWin < TcpSocketMgr::MSS_to_advertise ) {/' /opt/mtcp/tcplib/tcp.cpp \
 && grep -q 'origWin < TcpSocketMgr::MSS_to_advertise' /opt/mtcp/tcplib/tcp.cpp

# NASM assembles Polaris' boot sectors, startup code and resident INT 13h driver.
RUN apt-get update \
 && apt-get install -y --no-install-recommends nasm \
 && rm -rf /var/lib/apt/lists/*

ENV WATCOM=/opt/watcom \
    MTCP=/opt/mtcp \
    INCLUDE=/opt/watcom/h \
    EDPATH=/opt/watcom/eddat \
    WIPFC=/opt/watcom/wipfc \
    PATH=/opt/watcom/binl64:/opt/watcom/binl:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

WORKDIR /src
CMD ["make"]
