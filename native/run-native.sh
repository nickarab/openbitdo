#!/bin/sh
# Lançador do 8BitDo Ultimate nativo (GTK4) — pensado para Hyprland/tiling.
#
# GSK_RENDERER=gl: nesta máquina o layer Vulkan implícito (liblsfg-vk-layer.so)
# está quebrado e o renderer Vulkan do GTK4 derruba o app. GL é estável.
#
# BD_LAYOUT: força o split ("horizontal" | "vertical"). Sem ela, a divisão é
# automática pelo tamanho da janela (< 980px de largura ⇒ vertical).
set -e
cd "$(dirname "$0")"
export GSK_RENDERER="${GSK_RENDERER:-gl}"
exec ./8bdgui "$@"
