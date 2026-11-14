if sh src/arrow/tests/run-host.sh >"$tmp/arrow-host.log" 2>&1; then
    ok "Arrow protocol interoperability"
else
    bad "Arrow protocol interoperability"
    sed 's/^/        | /' "$tmp/arrow-host.log"
fi

if [ -f build/dos/bin/ARROWM.EXE ]; then
    arrow_ms_dir="$tmp/arrow-msclient"
    mkdir -p "$arrow_ms_dir"
    cp build/dos/bin/ARROWM.EXE "$arrow_ms_dir/"
    dos_run "$arrow_ms_dir" "ARROWM -?"
    if grep -qF "Usage: ARROWM" "$arrow_ms_dir/OUT.TXT" &&
       grep -qF "Requires running MSCLIENT TCP/IP" "$arrow_ms_dir/OUT.TXT"; then
        ok "Arrow MSCLIENT command-line help"
    else
        bad "Arrow MSCLIENT command-line help"
    fi
    dos_run "$arrow_ms_dir" "ARROWM -c 192.0.2.1 -b"
    if grep -qF "MSCLIENT TCP/IP unavailable" "$arrow_ms_dir/OUT.TXT"; then
        ok "Arrow MSCLIENT missing resident stack"
    else
        bad "Arrow MSCLIENT missing resident stack"
    fi
fi

if [ -f "${MSSDK:-/opt/mstcpsdk}/include/sys/socket.h" ]; then
    if sh src/arrow/tests/run-msclient.sh >"$tmp/arrow-msclient.log" 2>&1; then
        ok "Arrow MSCLIENT socket backend"
    else
        bad "Arrow MSCLIENT socket backend"
        sed 's/^/        | /' "$tmp/arrow-msclient.log"
    fi
fi
