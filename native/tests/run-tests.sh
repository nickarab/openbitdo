#!/bin/sh
# Suíte de regressão do 8bdgui (nativo, sem Wine).
#
# Uso: ./tests/run-tests.sh [-v]        (-v liga o traço do shell)
#      BD_SKIP_HARDWARE=1 ./tests/run-tests.sh
set -u
cd "$(dirname "$0")/.." || exit 1

if [ "${1:-}" = "-v" ]; then set -x; fi

PASS=0
FAIL=0
SKIP=0

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m  %s\n' "$1"; [ -n "${2:-}" ] && printf '        %s\n' "$2"; }
skip() { SKIP=$((SKIP+1)); printf '  \033[33mSKIP\033[0m  %s\n' "$1"; }

CC=${CC:-gcc}
# -Werror: o contrato do repositório é compilar sem warning, não só sem erro.
CFLAGS="-std=c23 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
-Wsign-conversion -Werror -I."
GTK=$(pkg-config --cflags --libs gtk4 2>/dev/null)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

# O Makefile decide a saída dos binários: build/ quanto o alvo cria o
# diretório, raiz de native/ enquanto a mudança não chega lá.
find_bin() {
    for dir in build .; do
        if [ -x "$dir/$1" ]; then
            printf '%s' "$dir/$1"
            return 0
        fi
    done
    return 1
}

echo "=== 1) núcleo (sem GTK, sem hardware) ==="
if $CC $CFLAGS -o "$OUT/test_core" tests/test_core.c bd_core.c 2>"$OUT/cc1.log"; then
    if "$OUT/test_core" > "$OUT/core.log" 2>&1; then
        summary=$(grep -E '^[0-9]+ checagens' "$OUT/core.log")
        ok "test_core: $summary"
    else
        bad "test_core falhou" "$(grep -E 'FAIL|falha' "$OUT/core.log" | head -5)"
    fi
else
    bad "test_core não compila" "$(head -5 "$OUT/cc1.log")"
fi

echo "=== 2) build do app (C23, sem warnings) ==="
if $CC $CFLAGS -o "$OUT/8bdgui" 8bdgui.c bd_core.c bd_hid.c $GTK -lm 2>"$OUT/cc2.log"; then
    ok "8bdgui compila (C23 + GTK4)"
else
    bad "8bdgui não compila" "$(grep -E 'error|warning' "$OUT/cc2.log" | head -5)"
fi

GUI_BIN=$(find_bin 8bdgui) || GUI_BIN="$OUT/8bdgui"

echo "=== 3) contrato do painel explicativo (sem display) ==="
# O painel textual é o que explica o protocolo ao usuário; ele não deve voltar
# a depender de arte raster nem de desenho manual via cairo/GdkTexture.
if grep -Eq 'assets/|\.png' 8bdgui.c; then
    bad "8bdgui.c ainda referencia assets/*.png"
else
    ok "8bdgui.c não referencia assets/*.png"
fi

if grep -Eq 'cairo_|GdkTexture|gdk_texture_' 8bdgui.c; then
    bad "8bdgui.c ainda chama cairo/GdkTexture"
else
    ok "8bdgui.c não chama cairo/GdkTexture"
fi

if grep -Fq 'build_info_panel' 8bdgui.c && grep -Fq 'build_tab_help' 8bdgui.c; then
    ok "painel explicativo montado por build_info_panel/build_tab_help"
else
    bad "painel explicativo (build_info_panel/build_tab_help) ausente"
fi

if grep -Eq 'GtkDrawingArea|gtk_drawing_area' 8bdgui.c; then
    bad "8bdgui.c ainda usa GtkDrawingArea (desenho manual)"
else
    ok "8bdgui.c não usa GtkDrawingArea (painel é texto/GTK nativo)"
fi

headings_missing=0
for heading in 'O que cada aba faz' 'Limites conhecidos'; do
    grep -Fq "$heading" 8bdgui.c || headings_missing=1
done
if [ "$headings_missing" = "0" ]; then
    ok "seções explicativas: abas e limites"
else
    bad "seção explicativa ausente (abas ou limites)"
fi

# Cada aba da barra precisa de uma linha em TAB_HELP. O static_assert do
# 8bdgui.c garante o número; aqui garantimos que os nomes são os seis da UI.
tabs_missing=0
for tab in Mapping Sticks Triggers Vibration Macro Perfil; do
    grep -Fq "\"$tab\"" 8bdgui.c || tabs_missing=1
done
if [ "$tabs_missing" = "0" ]; then
    ok "TAB_HELP documenta as 6 abas"
else
    bad "TAB_HELP não documenta todas as abas"
fi

echo "=== 4) ferramentas linux (bd_build/bd_cli) ==="
if [ ! -x tests/test_tools.sh ]; then
    bad "tests/test_tools.sh ausente ou não executável"
elif ./tests/test_tools.sh >"$OUT/tools.log" 2>&1; then
    tools_pass=$(grep -c '^PASS ' "$OUT/tools.log")
    if [ "$tools_pass" -eq 0 ]; then
        skip "ferramentas: $(grep -m1 '^SKIP ' "$OUT/tools.log" | cut -c7-)"
    else
        ok "test_tools: $(tail -n 1 "$OUT/tools.log")"
    fi
else
    bad "ferramentas linux falharam" "$(grep -m1 '^FAIL ' "$OUT/tools.log")"
fi

export DISPLAY="${DISPLAY:-:0}"
DISPLAY_OK=0
if command -v xwininfo >/dev/null && xwininfo -root >/dev/null 2>&1; then
    DISPLAY_OK=1
fi

echo "=== 5) latência do main loop (regressão do freeze) ==="
if [ "${BD_SKIP_HARDWARE:-0}" = "1" ]; then
    skip "latência do main loop: BD_SKIP_HARDWARE=1 (teste de hardware do dono)"
elif [ "$DISPLAY_OK" = "0" ]; then
    skip "sem display — teste de UI não roda"
elif [ ! -x "$GUI_BIN" ]; then
    bad "binário ausente para o teste de loop"
else
    pkill -x 8bdgui 2>/dev/null; sleep 1
    export GDK_BACKEND="${BD_TEST_BACKEND:-x11}"
    line=$(BD_LOOP_TEST=1 timeout 30 "$GUI_BIN" 2>&1 | grep -o 'LOOP max_gap_ms=[0-9]*' | tail -1)
    gap=${line#LOOP max_gap_ms=}
    if [ -z "$gap" ]; then
        bad "sonda de loop não respondeu (app não abriu?)" "$line"
    elif [ "$gap" -lt 250 ]; then
        ok "main loop responsivo (gap máximo ${gap} ms < 250 ms)"
    else
        bad "main loop travando: gap máximo ${gap} ms (I/O na thread da UI?)"
    fi
fi

echo "=== 6) smoke test da janela ==="
if [ "$DISPLAY_OK" = "0" ] || [ ! -x "$OUT/8bdgui" ]; then
    skip "sem display/binário"
else
    pkill -x 8bdgui 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        pgrep -x 8bdgui >/dev/null || break
        sleep 0.3
    done
    alive=0
    for attempt in 1 2; do
        GDK_BACKEND="${BD_TEST_BACKEND:-x11}" BD_NON_UNIQUE=1 \
            setsid "$OUT/8bdgui" > "$OUT/gui.log" 2>&1 &
        sleep 4
        if pgrep -x 8bdgui >/dev/null; then alive=1; break; fi
        pkill -x 8bdgui 2>/dev/null
        sleep 1
    done
    if [ "$alive" = "0" ]; then
        bad "app morreu no startup (2 tentativas)" "$(tail -3 "$OUT/gui.log")"
    elif command -v xwininfo >/dev/null; then
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
    else
        ok "processo vivo (xwininfo indisponível)"
    fi
    pkill -x 8bdgui 2>/dev/null; sleep 1
fi

echo "=== 7) device real (opcional) ==="
if [ "${BD_SKIP_HARDWARE:-0}" = "1" ]; then
    skip "hardware: BD_SKIP_HARDWARE=1 (teste do dono)"
elif ! command -v lsusb >/dev/null || ! lsusb 2>/dev/null | grep -qi '2dc8:'; then
    skip "nenhum 8BitDo 2dc8 conectado"
else
    # canal HID (DInput/software): leitura do perfil por qualquer PID 2dc8 com hidraw
    if [ ! -x "$GUI_BIN" ]; then
        skip "binário do app ausente para o HID"
    elif ! grep -qi 'HID_ID=.*2DC8' /sys/class/hidraw/hidraw*/device/uevent 2>/dev/null; then
        skip "sem hidraw 2dc8"
    elif timeout 60 "$GUI_BIN" --selftest >"$OUT/hid.log" 2>&1; then
        ok "HID: perfil lido ($(grep -c . "$OUT/hid.log") linhas de saída)"
    else
        bad "HID presente mas leitura do perfil falhou" "$(tail -2 "$OUT/hid.log")"
    fi
fi

echo
printf 'resultado: \033[32m%d pass\033[0m, \033[31m%d fail\033[0m, \033[33m%d skip\033[0m\n' \
       "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
