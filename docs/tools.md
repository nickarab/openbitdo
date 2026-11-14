# 17 — Ferramentas: GUI e linha de comando

Caminhos relativos à raiz do repositório; `make` gera tudo em `build/`. Os
comandos de hardware exigem o controle acessível como `/dev/hidraw*` (VID
`2dc8`) ou `2dc8:3106` no XInput, com a regra udev de `docs/udev.md`.

## `8bdgui` — GTK4

```bash
make                       # build/8bdgui, bd_cli, bd_build, bd_watch
native/run-native.sh       # GSK_RENDERER=gl, layout adaptável a tiling
```

Abas: **Mapping**, **Sticks**, **Triggers**, **Vibration**, **Macro** e
**Perfil**. O painel esquerdo lista as abas e os limites do
protocolo; o rodapé tem o seletor de hidraw, `Procurar` e a barra de status.

Fluxo de gravação:

1. `Procurar` enumera os hidraw `2dc8`.
2. `Reset` (ou `Ctrl+R`) lê o record de 1652 B do controle.
3. Ligar o switch **Permitir gravar** na barra superior.
4. `Sync to device` (ou `Ctrl+G`) grava chunks + commit. `--write-selftest`
   faz o caminho completo e relê para comparar.

Atalhos: `Ctrl+R` ler, `Ctrl+G` gravar/sincronizar, `Ctrl+P` procurar de novo.
Se o controle estiver em XInput (`2dc8:3106`, sem hidraw), o app manda o frame
de troca de modo pela interface vendor e espera ele voltar como `2dc8:3012`
(ver `docs/xinput-mode.md`).

Opções de linha de comando: `--auto-read` (lê 600 ms após abrir),
`--selftest` (lê e imprime o record), `--write-selftest ARQUIVO` (grava, relê e
compara ignorando `0x00C..0x00F` e `0x012..0x013`). Variáveis úteis:
`BD_LAYOUT` (`vertical`/`horizontal`), `BD_NON_UNIQUE=1` (permite uma segunda
instância) e `BD_LOOP_TEST=1` (sonda de latência usada pela suíte).

Exemplo real (usa o arquivo gerado na seção `bd_build`):

```bash
build/8bdgui --write-selftest /tmp/bd-base.bin
```

## `bd_cli` — list, dump, write, patch, info e troca de modo

Interface única sobre `native/bd_hid.h` (`bd_hid_find`, `bd_hid_open`,
`bd_hid_execute`, `bd_hid_read_record`, `bd_hid_write_record`).

```bash
build/bd_cli list
build/bd_cli dump ARQUIVO
build/bd_cli info
build/bd_cli write ARQUIVO [--verify] [--dry-run] [--no-commit] \
  [--first-chunk] [--pid N] [--device CAMINHO]
build/bd_cli patch SLOT [--p1 ACAO] [--p2 ACAO] [--mode 0..3]
build/bd_cli hid
build/bd_cli mode 1|2|3|4
build/bd_cli send-a CMD PARAM
```

| comando | efeito |
|---|---|
| `list` | enumera os hidraw `2dc8` |
| `dump ARQUIVO` | lê o record de 1652 B e grava no arquivo |
| `info` | `cmd 0x0002`: lê 20 B de estado e mostra slot + se o PID usa CRC de header |
| `write ARQUIVO` | grava chunks de 45 B (`cmd=0x0001`, ACK ecoa `sub=1`) e commit `cmd=0x0006/subcmd=0x0015` (ACK ecoa `cmd=6`) |
| `patch SLOT` | lê o record, altera só o slot (P1/P2, modo, nome `Profile N`), grava e relê; ativa o HID antes se o controle estiver em XInput |
| `hid` | em XInput, manda `81 05 00 51 01` pela interface vendor e espera o hidraw |
| `mode M` | `changeDinput_CMD`: 1 DInput, 2 XInput, 3 DS4, 4 Switch (pelo hidraw, ou pela interface vendor se não houver) |
| `send-a CMD PARAM` | comando Type A cru (`CMD` 0..255, `PARAM` u32) para experimentação |

`dump` e `info` usam o primeiro controle encontrado. Em `write`, `--device`
força o caminho e `--pid` força o PID (necessário se o caminho não estiver em
`/sys/class/hidraw`). `--verify` relê e compara ignorando os campos gerenciados
pelo device: `0x00C..0x00F` (`crc_value`, contador) e `0x012..0x013`
(`cur_slot`, que fica 4). `--dry-run` só mostra o resumo do arquivo;
`--no-commit` envia os chunks sem commit e `--first-chunk` para no primeiro.

Números inválidos (slot, modo, PID, `send-a`) são recusados com exit 2 antes
de procurar o controle.

Exemplo real:

```bash
$ build/bd_cli list
/dev/hidraw0  2dc8:3012
$ build/bd_cli dump /tmp/perfil.bin
/tmp/perfil.bin: 1652 bytes
$ build/bd_cli write /tmp/bd-base.bin --dry-run
/tmp/bd-base.bin: 1652 bytes | modo=1 | cur_slot=0 | crc_value=0x00000000
dry-run: nenhum comando enviado
```

Para gravar de verdade, troque `--dry-run` por `--verify` (ou nenhuma flag).

## `bd_build` — geração de record

Gera um record de 1652 B com nomes `Profile 1..3`, flags ligadas, ranges de
sticks e gatilhos em 0..255, mapa identidade nas 20 chaves e macros zeradas.
`crc_value` (0x00C) fica **zero**: é contador do device, que sobrescreve o valor
enviado. `cur_slot` (0x012) marca o slot editado no arquivo; o device mantém o
próprio valor (4).

```bash
build/bd_build SAIDA.bin [--from BASE.bin] [--mode N] [--slot N] [--p1 ACAO] [--p2 ACAO]
```

`--from` parte de um record existente (por exemplo um `bd_cli dump`) e altera
só o slot indicado. `--mode`: `0`=Switch, `1`=DInput (padrão), `2`=DS4,
`3`=XInput. `--slot` é `0..2`
(padrão `0`); P1/P2 são as entradas 18/19 e a sobrescrita vale só para o slot
escolhido. `--p1`/`--p2` aceitam nome (layout Xbox: `A`, `B`, `X`, `Y`, `LB`,
`RB`, `LT`, `RT`, `LS`, `RS`, `VIEW`, `MENU`, `HOME`, `STAR`, `BT_CON`,
`D_UP`, `D_DOWN`, `D_LEFT`, `D_RIGHT`, `SCREENSHOT`, `TURBO_SINGLE`,
`TURBO_AUTO`, `P1`, `P2`, `SWAP`), `none` ou valor numérico. Os nomes do vocabulário
8BitDo/Switch valem como alias: `L1`=`LB`, `R1`=`RB`, `L2`=`LT`, `R2`=`RT`,
`L3`=`LS`, `R3`=`RS`, `SELECT`=`VIEW`, `START`=`MENU`, `UP`/`DOWN`/`LEFT`/
`RIGHT`=`D_*`.

Atenção: no record, `RT` é o **gatilho direito** (0x8000); o d-pad → é
`D_RIGHT` (0x40). O índice 17 da tabela de chaves se chama `D_RIGHT` justamente
para não confundir os dois.

Exemplo real:

```bash
$ build/bd_build /tmp/bd-base.bin --mode 1 --slot 0 --p1 A --p2 B
gerado /tmp/bd-base.bin: 1652 bytes | modo=DInput slot=0 | P1=0x2000 P2=0x1000
```

## `bd_watch` — botões ao vivo

Mostra o estado dos botões A/B/X/Y do modo DInput (`0x3012`) lendo o report ID
3 do hidraw; o ID 2 (vendor) é descartado. O caminho é opcional e o padrão é
`/dev/hidraw0`. A linha só muda quando os bits de A/B/X/Y mudam; o restante do
report aparece em hex.

```bash
build/bd_watch /dev/hidraw0     # Ctrl+C para sair
```

## Nota comum de I/O

A gravação (`bd_cli write` e `8bdgui`) abre o hidraw com `O_NONBLOCK` e usa
`poll(POLLOUT)` antes de repetir `write()`: sem isso, o `write()` trava para
sempre quando o endpoint de saída do controle não é drenado. `bd_watch` também
usa `O_NONBLOCK`, mas apenas para leitura.
