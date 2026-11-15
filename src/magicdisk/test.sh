# Sourced by tests/run.sh; use its temporary directory and result helpers.
if make -C src/magicdisk test >"$tmp/magicdisk-host.log" 2>&1; then
    ok "MagicDisk filesystem and map fault-injection tests"
else
    bad "MagicDisk filesystem and map fault-injection tests"
    sed 's/^/        | /' "$tmp/magicdisk-host.log"
fi

if [ -f build/dos/bin/MAGICDSK.EXE ]; then
    if make -C src/magicdisk test-dos >"$tmp/magicdisk-dos.log" 2>&1; then
        ok "MagicDisk QEMU BIOS, protected-mode and TUI tests"
        sed -n '/^NOTE:/s/^/        | /p' "$tmp/magicdisk-dos.log"
    else
        bad "MagicDisk QEMU BIOS, protected-mode and TUI tests"
        sed 's/^/        | /' "$tmp/magicdisk-dos.log"
    fi
fi
