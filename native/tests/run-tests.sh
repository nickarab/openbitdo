#!/bin/sh
# Suíte de regressão do openbitdo.
#
# Uso: native/tests/run-tests.sh [-v]        (-v liga o traço do shell)
#      BD_SKIP_HARDWARE=1 native/tests/run-tests.sh
#
# Tudo é compilado pelo Makefile da raiz, com -Werror, num BUILD temporário:
# os testes exercitam os mesmos alvos que o `make` e o AppImage usam, sem
# mexer no build/ do usuário.
set -u
ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd) || exit 1
cd "$ROOT" || exit 1

if [ "${1:-}" = "-v" ]; then set -x; fi

PASS=0
FAIL=0
SKIP=0

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m  %s\n' "$1"; [ -n "${2:-}" ] && printf '        %s\n' "$2"; }
skip() { SKIP=$((SKIP+1)); printf '  \033[33mSKIP\033[0m  %s\n' "$1"; }

OUT=$(mktemp -d "${TMPDIR:-/tmp}/bd_suite.XXXXXX") || exit 1
trap 'rm -rf "$OUT"' EXIT HUP INT TERM
BIN="$OUT/build"

finish() {
    echo
    printf 'resultado: \033[32m%d pass\033[0m, \033[31m%d fail\033[0m, \033[33m%d skip\033[0m\n' \
           "$PASS" "$FAIL" "$SKIP"
    [ "$FAIL" -eq 0 ] || exit 1
    exit 0
}

echo "=== 1) build (C23, -Werror) ==="
if make -s BUILD="$BIN" CFLAGS="-O2 -Werror" all tests >"$OUT/make.log" 2>&1; then
    ok "make all tests: 4 binários + 2 testes sem warning"
else
    bad "build falhou" "$(grep -m3 -E 'error|warning' "$OUT/make.log")"
    finish
fi

run_unit() {
    if "$BIN/$1" >"$OUT/$1.log" 2>&1; then
        ok "$1: $(grep -E '^[0-9]+ checagens' "$OUT/$1.log")"
    else
        bad "$1 falhou" "$(grep -m5 'FAIL' "$OUT/$1.log")"
    fi
}

echo "=== 2) núcleo (sem GTK, sem hardware) ==="
run_unit test_core

echo "=== 3) transporte HID (socketpair, sem hardware) ==="
run_unit test_hid

echo "=== 4) painel explicativo da GUI ==="
# Cada aba precisa de uma linha em TAB_HELP; o static_assert do 8bdgui.c
# garante o número, aqui garantimos que os nomes são os seis da UI.
tabs_missing=""
for tab in Mapping Sticks Triggers Vibration Macro Perfil; do
    grep -Fq "\"$tab\"" native/8bdgui.c || tabs_missing="$tabs_missing $tab"
done
if [ -z "$tabs_missing" ]; then
    ok "TAB_HELP documenta as 6 abas"
else
    bad "TAB_HELP sem:$tabs_missing"
fi

echo "=== 5) ferramentas (bd_build/bd_cli) ==="
if BD_BIN_DIR="$BIN" native/tests/test_tools.sh >"$OUT/tools.log" 2>&1; then
    ok "test_tools: $(tail -n 1 "$OUT/tools.log")"
else
    bad "test_tools falhou" "$(grep -m3 '^FAIL ' "$OUT/tools.log")"
fi

export DISPLAY="${DISPLAY:-:0}"
DISPLAY_OK=0
if command -v xwininfo >/dev/null && xwininfo -root >/dev/null 2>&1; then
    DISPLAY_OK=1
fi
GUI_BIN="$BIN/8bdgui"

echo "=== 6) latência do main loop (regressão do freeze) ==="
if [ "${BD_SKIP_HARDWARE:-0}" = "1" ]; then
    skip "latência do main loop: BD_SKIP_HARDWARE=1"
elif [ "$DISPLAY_OK" = "0" ]; then
    skip "sem display X"
else
    pkill -x 8bdgui 2>/dev/null; sleep 1
    line=$(GDK_BACKEND="${BD_TEST_BACKEND:-x11}" BD_LOOP_TEST=1 timeout 30 "$GUI_BIN" 2>&1 |
           grep -o 'LOOP max_gap_ms=[0-9]*' | tail -1)
    gap=${line#LOOP max_gap_ms=}
    if [ -z "$gap" ]; then
        bad "sonda de loop não respondeu (app não abriu?)"
    elif [ "$gap" -lt 250 ]; then
        ok "main loop responsivo (gap máximo ${gap} ms < 250 ms)"
    else
        bad "main loop travando: gap máximo ${gap} ms (I/O na thread da UI?)"
    fi
fi

echo "=== 7) smoke test da janela ==="
if [ "$DISPLAY_OK" = "0" ]; then
    skip "sem display X"
else
    pkill -x 8bdgui 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        pgrep -x 8bdgui >/dev/null || break
        sleep 0.3
    done
    alive=0
    for _ in 1 2; do
        GDK_BACKEND="${BD_TEST_BACKEND:-x11}" BD_NON_UNIQUE=1 \
            setsid "$GUI_BIN" >"$OUT/gui.log" 2>&1 &
        sleep 4
        if pgrep -x 8bdgui >/dev/null; then alive=1; break; fi
        pkill -x 8bdgui 2>/dev/null
        sleep 1
    done
    if [ "$alive" = "0" ]; then
        bad "app morreu no startup (2 tentativas)" "$(tail -3 "$OUT/gui.log")"
    else
        found=0
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            if xwininfo -root -tree 2>/dev/null | grep -q 'Software'; then found=1; break; fi
            sleep 0.5
        done
        if [ "$found" = "1" ]; then
            ok "janela abriu e processo vivo"
        else
            bad "processo vivo mas nenhuma janela mapeada"
        fi
    fi
    pkill -x 8bdgui 2>/dev/null; sleep 1
fi

echo "=== 8) device real (opcional) ==="
if [ "${BD_SKIP_HARDWARE:-0}" = "1" ]; then
    skip "hardware: BD_SKIP_HARDWARE=1"
elif ! grep -qi 'HID_ID=.*2DC8' /sys/class/hidraw/hidraw*/device/uevent 2>/dev/null; then
    skip "sem hidraw 2dc8"
elif timeout 60 "$GUI_BIN" --selftest >"$OUT/hid.log" 2>&1; then
    ok "HID: perfil lido ($(grep -c . "$OUT/hid.log") linhas de saída)"
else
    bad "HID presente mas leitura do perfil falhou" "$(tail -2 "$OUT/hid.log")"
fi

finish
