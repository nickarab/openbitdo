# Testes

Tudo roda a partir da raiz do repositório.

```bash
make test                          # suíte completa (native/tests/run-tests.sh)
BD_SKIP_HARDWARE=1 make test       # pula a latência do loop e o device real
native/tests/run-tests.sh -v       # mesmo, com set -x
make tests                         # só compila build/test_core e build/test_hid
make test-asan                     # unitários com AddressSanitizer + UBSan
```

`run-tests.sh` compila **pelo Makefile**, com `-Werror`, num `BUILD`
temporário: os blocos exercitam os mesmos alvos que `make` e o AppImage usam,
sem tocar no `build/` do usuário. Se o build falha, a suíte para ali com FAIL.
Sai com 1 se algum bloco falhar; SKIP não falha.

## Blocos de `run-tests.sh`

| # | bloco | precisa de |
|---|---|---|
| 1 | `make all tests` com `-std=c23 … -Wsign-conversion -Werror` | `gcc`, GTK4 |
| 2 | `test_core` — núcleo puro | — |
| 3 | `test_hid` — transporte HID sobre `socketpair` | — |
| 4 | `TAB_HELP` do `8bdgui.c` cita as 6 abas | `grep` |
| 5 | `test_tools.sh` com os binários do bloco 1 | `python3` |
| 6 | latência do main loop (`BD_LOOP_TEST=1`, gap < 250 ms) | display X |
| 7 | smoke da janela (processo vivo + `xwininfo`) | display X |
| 8 | `8bdgui --selftest` lê o record de um hidraw `2dc8` | controle conectado |

`BD_SKIP_HARDWARE=1` pula os blocos 6 e 8. O bloco 7 só depende do display.

## `test_core` (`native/tests/test_core.c`)

Liga só `bd_core.c`. Os offsets do record são conferidos por `static_assert`
em `bd_core.c` e no topo do teste: mudar uma constante quebra o build.

| grupo | cobre |
|---|---|
| plano de chunks | `bd_chunk_len`: 36 × 45 + 32, 37 chunks, 0 no fim e além dele |
| crc16 | `"123456789"` → `0x916F`, vazio → `0xFFFF`, tabela de PIDs com CRC |
| record fields | `bd_rd*/bd_wr*` little-endian, posição de P1/P2 e do slot 1 |
| nomes UTF-16LE | acentos, corte em 15 code units, fora do BMP descartado sem sair dos 32 B do slot |
| tabelas | ações, `bd_key_short`/`bd_key_long`, `bd_action_value` (caixa, aliases, desconhecido), ida e volta nome → valor → índice |
| framing HID | frame `81 len+1 04`, limite de 61 B, parse de `02 04`, header Tipo C |

## `test_hid` (`native/tests/test_hid.c`)

Um `socketpair(AF_UNIX, SOCK_SEQPACKET)` faz o papel do `/dev/hidraw*`: cada
`write` é um report de 64 B com fronteira preservada. O lado "device"
enfileira as respostas antes e confere depois os frames que o host mandou.

| grupo | cobre |
|---|---|
| frames crus | `81 05 00 51 <m>` com o modo no byte 4 e zeros até 64 B; sessão (`81 05 00 21 01` + `0x07`/0, `0x07`/1 + `81 05 00 51 02`) |
| Type A | cmd em +0, param u32 LE em +4, sem resposta → `false` |
| Type C | ignora eco de outro cmd, copia de +18 limitado ao report e ao buffer, progress, CRC no header só nos PIDs da tabela, device mudo → `false` |
| read_record | 37 pedidos com offset/len certos, record igual ao do device, desiste após 2 tentativas no primeiro chunk |
| write_record | 37 chunks com os dados certos, commit `6`/`0x15`, 3 releituras de 20 B, commit sem eco → `false` |

Os casos de device mudo esperam os timeouts reais (150 ms × 4): o binário
leva ~4 s.

**Mutações verificadas** (cópia do repositório em `mktemp -d`): subcomando do
commit, offset do parâmetro do Type A, modo ignorado no `mode_switch`, eco de
outro cmd aceito, fast-fail 2 → 3, releituras 3 → 2, chunk 45 → 44 e CRC
ausente no `0x3109` — todas derrubam pelo menos uma checagem.

## `test_tools.sh`

Usa os binários de `BD_BIN_DIR` (o bloco 5 passa os do bloco 1); sozinho,
compila com o Makefile num diretório temporário. Build quebrado é FAIL.

- `bd_build`: record padrão (1652 B, feature mask ×3, flag da macro em
  `0x1DC`, nomes, modo/slot `1/0`, P1/P2, `crc_value = 0`); `--mode 3 --slot 2`
  mexe só no slot 2; `--from` preserva os slots não editados; rejeita ação
  inexistente, `--mode 4`, `--slot 3`, número inválido, opção sem valor ou
  desconhecida e `--from` ausente.
- `bd_cli`: sem argumentos → uso e exit 2; subcomando desconhecido e `mode 9`
  → exit 2; `write --dry-run` lê o arquivo sem enviar nada e rejeita record com
  tamanho errado; `list` não trava (limite de 5 s).

## Não coberto

- `bd_usb.c` (troca de modo pelo endpoint vendor do `3106`) só roda com o
  controle: usbfs não tem dublê simples.
- `bd_cli patch` lê e grava no device; a lógica de patch ainda não é uma
  função pura testável (fase 2).
- A semântica da sessão (`81 05 00 21 01` / `81 05 00 51 02`) está congelada
  como é hoje; mudar exige validar no hardware.
