#!/bin/sh
# usage: dos-run.sh <seconds> <shot.png|-> <dos command lines...>   (inside the test container)
# Boots DOSBox-X with an emulated NE2000 (slirp NAT, host = 10.0.2.2), loads the packet
# driver and runs the given commands.  With a shot name, grabs the screen before the end.
T=$1; SHOT=$2; shift 2
cd "$(dirname "$0")/../../.."
D=/tmp/dosc; rm -rf $D; mkdir -p $D
cp build/dos/bin/NETBOLTD.EXE /opt/dos/NE2000.COM $D/
printf 'PACKETINT 0x60\r\nIPADDR 10.0.2.15\r\nNETMASK 255.255.255.0\r\nGATEWAY 10.0.2.2\r\nNAMESERVER 10.0.2.3\r\nMTU 1500\r\n' > $D/MTCP.CFG
cat > /tmp/dbx.conf <<EOC
[sdl]
output=surface
[dosbox]
memsize=16
quit warning=false
[render]
aspect=false
scaler=none
[cpu]
cycles=max
[ne2000]
ne2000=true
nicbase=300
nicirq=3
backend=slirp
[ethernet, slirp]
tcp_port_forwards=5292:5201
[autoexec]
mount c $D
c:
ne2000 0x60 3 0x300 > NE.TXT
set MTCPCFG=MTCP.CFG
EOC
for l in "$@"; do echo "$l" >> /tmp/dbx.conf; done
echo exit >> /tmp/dbx.conf
export DISPLAY=:99 SDL_AUDIODRIVER=dummy
pgrep Xvfb >/dev/null || { Xvfb :99 -screen 0 800x600x24 >/dev/null 2>&1 & sleep 1; }
timeout $T dosbox-x -conf /tmp/dbx.conf -fastlaunch -nomenu >/tmp/dbx.log 2>&1 &
PID=$!
if [ "$SHOT" != "-" ]; then
    sleep $(( T - 3 ))
    import -window root -bordercolor black -border 1 -trim +repage "$SHOT"
    if [ -f "${SHOT%.png}-0.png" ]; then
        mv "${SHOT%.png}-0.png" "$SHOT"
        rm -f "${SHOT%.png}-1.png"
    fi
fi
wait $PID
cat $D/OUT*.TXT 2>/dev/null
exit 0
