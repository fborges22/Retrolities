# ShellIV GUI tests (sourced by tests/run.sh): runs tests/ops.sh (file
# operations) and tests/tour2.sh (Program Manager features) against the
# Windows 3.x (32-bit extender), Windows 9x and NT builds under Wine on a
# headless X server. Each script drives the program with the keyboard and
# mouse and writes one "ok" or "FAIL" line per check to a log.

if command -v Xvfb >/dev/null && command -v xdotool >/dev/null; then
    for sv_plat in win386 win9x winnt; do
        sv_exe=build/$sv_plat/bin/SHELLIV.EXE
        [ -f "$sv_exe" ] || continue
        for sv_test in ops tour2; do
            sv_log=build/shots/$sv_test.log
            rm -f "$sv_log"
            GUI_DESKTOP=none GUI_SCREEN=1024x768x24 GUI_WAIT=25 timeout 400 \
                tests/gui.sh "$sv_exe" src/shelliv/tests/$sv_test.sh >/dev/null 2>&1
            if [ ! -s "$sv_log" ]; then
                bad "shelliv $sv_plat $sv_test: no results"
                continue
            fi
            while read -r sv_state sv_what; do
                if [ "$sv_state" = ok ]; then
                    ok "shelliv $sv_plat: $sv_what"
                else
                    bad "shelliv $sv_plat: $sv_what"
                fi
            done < "$sv_log"
        done
    done
fi
