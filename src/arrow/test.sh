if sh src/arrow/tests/run-host.sh >"$tmp/arrow-host.log" 2>&1; then
    ok "Arrow protocol interoperability"
else
    bad "Arrow protocol interoperability"
    sed 's/^/        | /' "$tmp/arrow-host.log"
fi
