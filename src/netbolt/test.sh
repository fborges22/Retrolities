if sh src/netbolt/tests/run-host.sh >"$tmp/netbolt-host.log" 2>&1; then
    ok "NetBolt protocol interoperability"
else
    bad "NetBolt protocol interoperability"
    sed 's/^/        | /' "$tmp/netbolt-host.log"
fi
