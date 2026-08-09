#!/usr/bin/env bash
# ============================================================
# gate_selftest.sh — prove every gate can fail, without an API key.
#
# A keyed run costs money and ~30 minutes, so "can this assertion fail?" must
# be answerable without one. It is answerable, because a gate reads only G_OUT
# and G_TELE: point those at a crafted fixture instead of a live run's
# artifacts and the gate cannot tell the difference.
#
# For each registered gate:
#   fixtures/<ID>/pass/  -> the gate must emit exactly one PASS
#   fixtures/<ID>/fail/  -> the gate must emit exactly one FAIL
#
# Both directions are load-bearing. Without the fail/ half a gate that always
# passes looks perfect. Without the pass/ half a gate can be "fixed" by making
# it match nothing, which also always fails and is equally useless.
#
# A gate with no fail/ fixture is itself reported as a failure. That is the
# whole point: vacuity becomes impossible to ADD, rather than something a
# reviewer is expected to notice.
#
# USAGE
#   gate_selftest_run <fixtures_dir>     # after gate_init + gate_def + bodies
# ============================================================

# Run one gate against one fixture directory; echo the verdict it produced.
# Verdict is PASS/FAIL/SKIP/NONE/MULTI so the caller can insist on exactly one.
_gate_selftest_invoke() {
    local id="$1" dir="$2"
    G_OUT="$dir/stdout.txt"; G_TELE="$dir/telemetry.jsonl"
    [ -f "$G_OUT" ]  || : > "$G_OUT"
    [ -f "$G_TELE" ] || : > "$G_TELE"
    # A fixture describes the artifacts, not the run's liveness — clear the
    # gov-kill state so gk_fail does not silently convert an expected FAIL
    # into a SKIP and hide an unfailable gate.
    GOV_KILL=""; RUN_TRUNCATED=""
    local qsave="$GATE_QUIET"; GATE_QUIET=1
    gate_reset_counters
    gate_run "$id"
    GATE_QUIET="$qsave"
    local n=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
    if   [ "$n" -eq 0 ]; then echo "NONE"
    elif [ "$n" -gt 1 ]; then echo "MULTI"
    elif [ "$PASS_COUNT" -eq 1 ]; then echo "PASS"
    elif [ "$FAIL_COUNT" -eq 1 ]; then echo "FAIL"
    else echo "SKIP"; fi
}

# $1 = fixtures root. Returns non-zero if any gate is unproven.
gate_selftest_run() {
    local root="$1"
    local ids=("${GATE_IDS[@]}")
    local descs=("${GATE_DESCS[@]}")
    local bad=0 i id got

    echo ""
    echo -e "${CYAN}+==============================================================+${NC}"
    echo -e "${CYAN}|  Gate self-test — every gate must fail on its fail fixture   |${NC}"
    echo -e "${CYAN}+==============================================================+${NC}"
    echo ""

    if [ "${#ids[@]}" -eq 0 ]; then
        echo -e "  ${RED}NO GATES REGISTERED${NC} — nothing to prove"
        return 1
    fi

    for i in "${!ids[@]}"; do
        id="${ids[$i]}"
        local fdir="$root/$id/fail" pdir="$root/$id/pass"

        if [ ! -d "$fdir" ]; then
            echo -e "  ${RED}UNPROVEN${NC} [$id] no fail/ fixture — this gate has never been seen to fail"
            bad=$((bad + 1)); continue
        fi

        got=$(_gate_selftest_invoke "$id" "$fdir")
        if [ "$got" != "FAIL" ]; then
            echo -e "  ${RED}NOT FAILABLE${NC} [$id] fail/ fixture produced $got, expected FAIL"
            bad=$((bad + 1)); continue
        fi

        if [ -d "$pdir" ]; then
            got=$(_gate_selftest_invoke "$id" "$pdir")
            if [ "$got" != "PASS" ]; then
                echo -e "  ${RED}NOT SATISFIABLE${NC} [$id] pass/ fixture produced $got, expected PASS"
                bad=$((bad + 1)); continue
            fi
            echo -e "  ${GREEN}PROVEN${NC} [$id] fails on fail/, passes on pass/"
        else
            echo -e "  ${YELLOW}PARTIAL${NC} [$id] fails on fail/; no pass/ fixture, so it could be matching nothing"
        fi
    done

    # Registry lint: an id emitted by a gate body but never declared is a gate
    # nobody can prove, and a declared id no run emits is a gate that has
    # silently stopped existing. Both are invisible without this check.
    local src lint_bad=0
    for src in "${GATE_SOURCES[@]:-}"; do
        [ -f "$src" ] || continue
        while IFS= read -r lit; do
            [ -n "$lit" ] || continue
            if ! gate_registered "$lit"; then
                echo -e "  ${RED}UNREGISTERED${NC} [$lit] emitted by $(basename "$src") but never gate_def'd"
                lint_bad=$((lint_bad + 1))
            fi
        done < <(grep -ohE '\b(pass|fail|skip|gk_fail) +"?[A-Za-z0-9][A-Za-z0-9._-]*"?' "$src" \
                 | awk '{print $2}' | tr -d '"' | sort -u)
    done
    bad=$((bad + lint_bad))

    echo ""
    if [ "$bad" -eq 0 ]; then
        echo -e "  ${GREEN}All ${#ids[@]} gates proven failable.${NC}"
        return 0
    fi
    echo -e "  ${RED}$bad gate(s) unproven.${NC} A gate that cannot fail is not a test."
    return 1
}
