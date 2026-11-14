#!/bin/sh
# Scaffold a new utility:  tools/newproj.sh <name> [console|gui]
set -eu
cd "$(dirname "$0")/.."

name=${1:-}
type=${2:-console}

if ! printf '%s' "$name" | grep -Eq '^[a-z0-9_-]{1,8}$'; then
    echo "usage: $0 <name> [console|gui]   (name: 1-8 chars of a-z 0-9 _ -)" >&2
    exit 1
fi
case $type in console|gui) ;; *) echo "type must be 'console' or 'gui'" >&2; exit 1 ;; esac

dir=src/$name
if [ -e "$dir" ]; then
    echo "$dir already exists" >&2
    exit 1
fi
mkdir -p "$dir"

if [ "$type" = console ]; then
    cat > "$dir/project.mk" <<EOT
NAME := $name
TYPE := console
EOT
    cat > "$dir/$name.c" <<EOT
#include <stdio.h>
#include "retro.h"

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    printf("$name for %s\n", retro_target());
    return 0;
}
EOT
    printf '%s for \n' "$name" > "$dir/test.expect"
else
    cat > "$dir/project.mk" <<EOT
NAME      := $name
TYPE      := gui
PLATFORMS := win16 win9x winnt
EOT
    cat > "$dir/$name.c" <<EOT
#include <windows.h>
#include "retro.h"

int PASCAL WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)inst; (void)prev; (void)cmdline; (void)show;

    MessageBox(NULL, retro_target(), "$name", MB_OK);
    return 0;
}
EOT
fi

echo "created $dir ($type)"
