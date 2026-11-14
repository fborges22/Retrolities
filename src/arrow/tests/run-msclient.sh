#!/bin/sh
# Requires user-supplied SDK headers, but no Microsoft runtime or library.
set -eu
cd "$(dirname "$0")/../../.."
MSSDK=${MSSDK:-/opt/mstcpsdk}
if [ ! -f "$MSSDK/include/sys/socket.h" ]; then
    echo "Set MSSDK=<Microsoft TCP/IP Sockets SDK root>" >&2
    exit 1
fi
T=$(mktemp -d)
trap 'rm -f "$T/msclient-test"; rmdir "$T"' EXIT
${CC:-cc} -std=c99 -O2 -Wall -Wextra -Werror -fno-builtin \
    -Dfar= -Dcdecl= -D__far= -D__interrupt= -DIP3_MAX_STREAMS=4 \
    -Isrc/arrow/tests/msclient -I"$MSSDK/include" -Isrc/arrow/common \
    -o "$T/msclient-test" src/arrow/tests/msclient-test.c
"$T/msclient-test"
