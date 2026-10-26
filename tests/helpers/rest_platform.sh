#!/usr/bin/env bash
# tests/helpers/rest_platform.sh -- where REST /execute cannot run a program.
#
# Since Bundle 2 each /execute request runs in a `naab-lang run` worker
# process, which needs POSIX process groups and pipes. On Windows the server
# answers 501 "Not available on this platform yet" instead (owner's decision
# 2026-10-26, like `naab-lang mcp`). An arm that needs /execute to RUN a
# program is UNMEASURABLE there; an arm that tests only authentication still
# measures it, because the 501 comes after the key is accepted (a refused key
# is 401 or 403).
#
#   rest_execute_unavailable        true on Windows (MSYS2 / MinGW / Cygwin)
#   REST_EXEC_OK_STATUS             200, or 501 where /execute cannot run
#   REST_EXEC_UNAVAILABLE_MSG       the 501 body's error text
#   rest_execute_is_501 CODE BODY   true when a response is that documented 501
#
# Use rest_execute_is_501 rather than the platform alone before reporting an
# arm UNMEASURABLE: a server that broke in some other way must still fail.

rest_execute_unavailable() {
    case "$(uname -s 2>/dev/null)" in
        MINGW*|MSYS*|CYGWIN*) return 0 ;;
    esac
    return 1
}

REST_EXEC_UNAVAILABLE_MSG="Not available on this platform yet"
if rest_execute_unavailable; then REST_EXEC_OK_STATUS=501; else REST_EXEC_OK_STATUS=200; fi

rest_execute_is_501() {  # rest_execute_is_501 HTTP_CODE BODY
    [ "${1%$'\r'}" = 501 ] && grep -qF "$REST_EXEC_UNAVAILABLE_MSG" <<<"$2"
}
