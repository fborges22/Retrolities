# Sourced by tests/run.sh; use its temporary directory and result helpers.
if [ -f build/polaris/stage2.bin ]; then
    if python3 src/polaris/tests/test_units.py >"$tmp/polaris-units.log" 2>&1; then
        ok "Polaris PLZ codec, image layout, install/uninstall"
    else
        bad "Polaris PLZ codec, image layout, install/uninstall"
        sed 's/^/        | /' "$tmp/polaris-units.log"
    fi
fi

# Booting real systems needs a FreeDOS 1.4 boot floppy image:
#   POLARIS_FREEDOS=path/to/freedos.img docker compose run --rm test
if [ -n "$POLARIS_FREEDOS" ] && [ -f build/dos/bin/POLARIS.EXE ]; then
    if (cd src/polaris/tests && python3 mkdemo.py "$POLARIS_FREEDOS" && python3 boot_test.py &&
        python3 installer_test.py "$POLARIS_FREEDOS") >"$tmp/polaris-qemu.log" 2>&1; then
        ok "Polaris QEMU boots: partitions, SCSI swap, El Torito CD, installer"
    else
        bad "Polaris QEMU boots: partitions, SCSI swap, El Torito CD, installer"
        sed 's/^/        | /' "$tmp/polaris-qemu.log"
    fi
fi
