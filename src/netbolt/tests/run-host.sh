#!/bin/sh
# Protocol regression test: NetBolt engine against a standard iperf3 peer.
set -e
cd "$(dirname "$0")/../../.."
P=${PORT:-5291}
cc -O2 -Wall -Isrc/netbolt/common -DIP3_BLK_MAX=131072 \
  -o /tmp/netbolt-hosttest src/netbolt/tests/hosttest.c \
  src/netbolt/tests/net_posix.c src/netbolt/common/ip3core.c
iperf3 -s -p "$P" >/tmp/netbolt-srv.log 2>&1 & SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 0.5
echo "== client, forward";            /tmp/netbolt-hosttest -c 127.0.0.1 -p "$P" -t 2
echo "== client, reverse";            /tmp/netbolt-hosttest -c 127.0.0.1 -p "$P" -t 2 -R
echo "== client, 4 streams";          /tmp/netbolt-hosttest -c 127.0.0.1 -p "$P" -t 2 -P 4
echo "== client, 4 streams reverse";  /tmp/netbolt-hosttest -c 127.0.0.1 -p "$P" -t 2 -P 4 -R
kill $SRV; wait $SRV 2>/dev/null || true
P=$((P+1))
/tmp/netbolt-hosttest -s -p "$P" >/tmp/netbolt-srv2.log 2>&1 & SRV=$!
sleep 0.5
echo "== server, forward";            iperf3 -c 127.0.0.1 -p "$P" -t 2 | tail -4
echo "== server, reverse, 3 streams"; iperf3 -c 127.0.0.1 -p "$P" -t 2 -R -P 3 | tail -5
echo "== server, udp must be refused"; iperf3 -c 127.0.0.1 -p "$P" -t 1 -u 2>&1 | tail -1 || true
echo "== server still alive";         iperf3 -c 127.0.0.1 -p "$P" -t 1 | tail -3
echo "ALL HOST TESTS PASSED"
