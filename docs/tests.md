# 18 — Matriz de testes atual (snapshot 14/11 01:29)

Substitui `12-testes.md`, que fica como **histórico** (lá: 142 checagens e 6
verificações; os números não valem mais). Caminhos relativos a `native/`.

## Como rodar

```bash
cd native && make test                    # = make all + ./tests/run-tests.sh
cd native && ./tests/run-tests.sh         # sem rebuild do app
cd native && ./tests/run-tests.sh -v      # liga set -x
BD_SKIP_HARDWARE=1 ./tests/run-tests.sh   # pula blocos 6 e 8
```

O script recompila tudo em `$TMPDIR` com
`-std=c23 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror -I.`;
não usa os binários de `make` para compilar (eles só servem aos blocos 6/7, e
`find_bin` procura em `build/` antes da raiz de `native/`). Sai com 1 se algum
bloco der FAIL — SKIP não falha. Última linha: `resultado: N pass, M fail, K skip`.

## `tests/test_core.c` — 275 checagens, 8 grupos

Liga só `bd_core.c`: sem GTK, sem libusb, sem hidraw. Quatro `static_assert` no
topo (record `0x674`, report 64, header 16, 20 teclas) quebram o build antes de
rodar se uma constante de protocolo mudar.

| grupo | cobre |
|---|---|
| record layout | offsets `0x00c 0x010 0x012 0x014 0x074 0x098 0x0b0 0x0c8 0x0e0 0x1dc`; `map_key` 84 B × 3 e `macro_fun` 392 B × 3 fechando em `0x674` |
| plano de chunks | 1652 = 36×45 + 32; último chunk de 32 B; 37 envios |
| crc16 | `"123456789"` → `0x916F`, vazio → `0xFFFF`, 8×`0xFF` ≠ `0xFFFF`, `bd_pid_uses_crc` (0x3109/0x3011 sim, 0x3012/0x3106 não) |
| record fields | little-endian em +0/+1, `0x00c`, flag de 4 B antes do mapa, P1/P2 em 18/19, slot 1 a +84 B, sticks/triggers, float do rumble (roundtrip) |
| nomes UTF-16LE | acentos (`Ação`), corte em 15 code units, U+1F600 descartado sem sair do slot de 32 B (canário `0xA5`), slots isolados |
| tabelas | 26 ações, `0x2000`→A, `0x4000000`→P2, desconhecido→0; `bd_key_short` 16=LF, 17=RT (d-pad →), 18=P1, 19=P2; modos 1=DInput, 3=XInput |
| framing HID | quadro `81 len+1 04`, 19 B com header de 16, padding zero até 64, payload máx 61 (62 rejeitado), vazio = 3 B, parse da resposta em +18, header Tipo C LE (cmd/sub/len/CRC em +6/total/offset) |
| XUSB / XInput | report neutro, todos os bits de botão, sticks assinados + inversão de Y, rejeita `data[0]!=0` e report curto, layout do rumble (byte alto) |

## `tests/run-tests.sh` — 8 blocos

| # | bloco | precisa de |
|---|---|---|
| 1 | núcleo: `test_core` (275 checagens) | `gcc` |
| 2 | build do app em `$TMPDIR` (C23 + GTK4 + libusb, `-Werror`) | `pkg-config` gtk4 + libusb-1.0 |
| 3 | contrato do painel: 6 greps em `8bdgui.c` — sem `assets/*.png`, sem `cairo_`/`GdkTexture`, sem `GtkDrawingArea`, `build_info_panel`+`build_tab_help`, 3 títulos, as 7 abas em `TAB_HELP` | `grep` |
| 4 | artes `assets/{front,sticks,back,vibration,macro}.png` em 828×664 (aceita 829×666) | `python3` + Pillow |
| 5 | `tests/test_tools.sh` (ferramentas linux) | ver abaixo |
| 6 | latência do main loop com XInput ativo (`BD_LOOP_TEST=1`, `LOOP max_gap_ms` < 250) | display X + binário |
| 7 | smoke da janela: processo vivo + `xwininfo` vê a janela (sem `xwininfo`, basta o processo vivo) | display X + binário |
| 8 | device real: XUSB `0x3106` (`xsusb --rumble 0 0`) e HID (`8bdgui --selftest` lê o record) | `lsusb` + hidraw `2dc8` |

O bloco 6 mede o maior intervalo entre iterações do `GMainLoop` com XInput
ligado: I/O de hidraw na thread da UI estoura o limite (o sintoma original dava
gaps de centenas de ms e travas de ~94 s).

## `tests/test_tools.sh` — offline, 7 checagens

Não abre `/dev/hidraw*`: `bd_build` só escreve arquivo e `bd_cli list` enumera
sysfs. Compila `linux/bd_build.c` e `linux/bd_cli.c` contra `bd_core.c` +
`bd_hid.c` com o mesmo C23 e `-Wsign-conversion` (warning = FAIL) e valida o
record com `python3 struct`:

- 1652 B, `flags=1` ×3, nomes `Profile 1..3`, mode/slot `1/0`;
- P1=`0x2000`/P2=`0x1000` em +18/+19 e `crc_value=0` — em `0x00C` o device
  sobrescreve com contador, então a ferramenta não inventa o campo;
- `--mode 3 --slot 2` desloca o mapa em 84×2 B e mantém P1/P2;
- ação inexistente → exit ≠ 0; `bd_cli` sem argumentos → exit 2 com uso;
  `bd_cli list` não trava (limite de 5 s).

Fonte ausente ou build quebrado = SKIP, para separar "lane não entregue" de
regressão real.

## Teste de mutação (a suíte tem dentes)

Rodado numa cópia do par `bd_core.{c,h}` + `tests/`, em `mktemp -d` — o
repositório não é editado. Baseline: 275 checagens, 0 falha.

```bash
cd native && M=$(mktemp -d) && mkdir -p "$M/tests" \
  && cp bd_core.c bd_core.h "$M"/ && cp tests/test_core.c "$M/tests"/
cd "$M" && sed -i 's/CRC16_REFLECTED_POLYNOMIAL = 0x8408;/CRC16_REFLECTED_POLYNOMIAL = 0x8400;/' bd_core.c
gcc -std=c23 -O2 -I. -o t tests/test_core.c bd_core.c && ./t | tail -1
# 275 checagens, 1 falha(s)
```

| mutação | resultado medido |
|---|---|
| polinômio do CRC `0x8408` → `0x8400` | 1 falha (vetor `"123456789"`) |
| P1/P2 trocados em `bd_key_short` (índices 18/19) | 2 falhas (`test_core.c:243-244`) |
| P1/P2 trocados em `bd_actions` (máscaras P1↔P2) | 1 falha (`test_core.c:239`, `bd_action_index(0x4000000)`) |

As duas tabelas precisam do teste: trocar as máscaras só aparece no índice da
ação, e trocar os nomes curtos só aparece em `bd_key_short`.

## Skips e limites

- Bloco 6: `BD_SKIP_HARDWARE=1` ou sem display X (`xwininfo -root` falha).
  Binário ausente ali é **FAIL**, não SKIP.
- Bloco 7: sem display X ou sem binário. `BD_SKIP_HARDWARE` **não** o cobre —
  ele sobe o `8bdgui` por 4 s (binário de `build/`, de `native/` ou o compilado
  no bloco 2) e mata o processo com `pkill` no fim.
- Bloco 8: `BD_SKIP_HARDWARE=1`, sem `lsusb`/`2dc8`, `0x3106` ausente (XUSB),
  `xsusb` não compilado, sem hidraw `2dc8` (HID). Sem permissão para abrir o
  hidraw o resultado é **FAIL** ("HID presente mas leitura do perfil falhou").
- Offline puro (blocos 1–5) medido com `BD_SKIP_HARDWARE=1` e um `xwininfo`
  falso no `PATH`: `10 pass, 0 fail, 3 skip`.
- O alvo `8bdgui` do `Makefile` não usa `-Wsign-conversion` nem `-Werror`; quem
  impede warning no app é o bloco 2. O `xsusb` é compilado com
  `-Wno-unused-variable` (tabela legada).
- `bd_hid.c` não tem teste unitário: abrir, `O_NONBLOCK`+`poll(POLLOUT)`, ACK e
  commit só são exercitados no bloco 8, com hardware.
