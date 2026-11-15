# Retrolities test image: QEMU (MagicDisk DOS), DOSBox-X (other DOS), Wine (Windows)
FROM debian:trixie-slim

RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates build-essential curl unzip iperf3 python3 dosbox-x wine wine32:i386 wine64 \
      xvfb xauth xdotool imagemagick dosfstools mtools qemu-system-x86 \
 && rm -rf /var/lib/apt/lists/*

# FreeDOS Crynwr packet driver for DOSBox-X's emulated NE2000 adapter.
RUN mkdir -p /opt/dos \
 && curl -fsSL https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.2/net/crynwr.zip \
      -o /tmp/crynwr.zip \
 && unzip -p /tmp/crynwr.zip DRIVERS/CRYNWR/NE2000.COM > /opt/dos/NE2000.COM \
 && unzip -p /tmp/crynwr.zip DOC/CRYNWR/GPL.DOC > /opt/dos/NE2000-LICENSE.TXT \
 && chmod 644 /opt/dos/NE2000.COM /opt/dos/NE2000-LICENSE.TXT \
 && rm /tmp/crynwr.zip

ENV SDL_VIDEODRIVER=dummy \
    SDL_AUDIODRIVER=dummy \
    WINEDEBUG=-all \
    WINEDLLOVERRIDES=mscoree,mshtml=

WORKDIR /src
CMD ["tests/run.sh"]
