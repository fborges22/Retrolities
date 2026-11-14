# Retrolities test image: headless DOSBox-X (MS-DOS) + Wine (Win16 / Win32)
FROM debian:trixie-slim

RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates make python3 dosbox-x wine wine32:i386 \
      xvfb xauth xdotool imagemagick \
 && rm -rf /var/lib/apt/lists/*

ENV SDL_VIDEODRIVER=dummy \
    SDL_AUDIODRIVER=dummy \
    WINEDEBUG=-all \
    WINEDLLOVERRIDES=mscoree,mshtml=

WORKDIR /src
CMD ["tests/run.sh"]
