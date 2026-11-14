# 10 — 8bdgui em Hyprland / tiling (adaptação de janela)

## Por que a janela parecia "quebrada"

Hyprland **tileia** por padrão. O pedido de tamanho do app
(`gtk_window_set_default_size`) é ignorado: na sessão de teste o compositor
impôs 1268×1314 (= metade de 2560×1440 com painel). Sintoma: arte pequena
flutuando num painel enorme e colunas de rótulo brigando com a imagem.

Comprovado: `min/nat` do conteúdo = 1239×730, mas `gtk_widget_get_width()` já
retornava 1268 — isto é, o tamanho vem do compositor, não do GTK.

## O que foi feito

1. **Layout responsivo** (`adapt_layout_cb`, a cada 250 ms):
   - largura < **980 px** → `GtkPaned` em **vertical**: arte em cima (largura toda), tabela embaixo rolando;
   - largura ≥ 980 px → lado a lado como no app oficial.
   - O `GtkPaned` é reposicionado (45% horizontal / 52% vertical) na troca.
2. **Arte escala com o espaço**: usa o maior ajuste que couber
   (`min(largura - 190px de rótulos, altura * 0.80)`), sem teto rígido.
3. **Rótulos somem** quando o painel fica menor que 360 px (evita poluição).
4. **Sem `size_request` grande**: mínimo do painel agora é 260×200, então tile
   pequeno não estoura mais a janela.
5. **`BD_LAYOUT=vertical|horizontal`** força a divisão (debug/uso manual).
6. **`run-native.sh`** exporta `GSK_RENDERER=gl` — o layer Vulkan implícito
   `liblsfg-vk-layer.so` desta máquina falha (`vkGetInstanceProcAddr` ausente) e
   o renderer Vulkan do GTK4 **derrubou o app** uma vez; com GL é estável.

## app_id para regras do Hyprland

```
class: org.rea.eightbdoultimate
```

Regras sugeridas (`~/.config/hypr/hyprland.conf` ou um `conf.d/*.conf`):

```ini
# flutuar num tamanho fixo (mais parecido com o app oficial)
windowrulev2 = float,          class:^(org.rea.eightbdoultimate)$
windowrulev2 = size 1280 820,  class:^(org.rea.eightbdoultimate)$
windowrulev2 = center,         class:^(org.rea.eightbdoultimate)$

# OU deixar tilear (o layout se adapta sozinho) com um mínimo sadio:
windowrulev2 = minsize 520 420, class:^(org.rea.eightbdoultimate)$
```

Comandos úteis:

```bash
hyprctl clients -j | grep -A2 org.rea      # geometria/tile atual
hyprctl dispatch togglefloating class:org.rea.eightbdoultimate
hyprctl dispatch resizeactive exact 900 800
BD_LAYOUT=vertical ./native/run-native.sh  # força split vertical
```

## Estado verificado

| cenário | resultado |
|---|---|
| janela larga (1268 px) | split horizontal, arte à esquerda + tabela à direita ✅ |
| `BD_LAYOUT=vertical` (917×950) | arte ocupa a largura toda em cima, tabela abaixo ✅ |
| painel < 360 px | callouts ocultados ✅ |
| Wayland nativo (Hyprland) | app sobe com `class=org.rea.eightbdoultimate`, tiled ✅ |
| renderer | Vulkan desta máquina quebra → `GSK_RENDERER=gl` no lançador ✅ |
