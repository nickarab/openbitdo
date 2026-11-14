#!/bin/sh
# Lançador do 8BitDo Ultimate nativo (GTK4) — pensado para Hyprland/tiling.
# Usa o binário do `make` na raiz (build/8bdgui).
#
# GSK_RENDERER=gl: nesta máquina o layer Vulkan implícito (liblsfg-vk-layer.so)
# está quebrado e o renderer Vulkan do GTK4 derruba o app. GL é estável.
#
# BD_LAYOUT: força o split ("horizontal" | "vertical"). Sem ela, a divisão é
# automática pelo tamanho da janela (< 980px de largura ⇒ vertical).
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
[ -x "$ROOT/build/8bdgui" ] || { echo "build/8bdgui ausente: rode 'make' na raiz" >&2; exit 1; }
export GSK_RENDERER="${GSK_RENDERER:-gl}"
exec "$ROOT/build/8bdgui" "$@"
