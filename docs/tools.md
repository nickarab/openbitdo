# 17 — Ferramentas: GUI e linha de comando

Caminhos relativos à raiz do repositório. Os comandos de hardware exigem o
controle acessível como `/dev/hidraw*` (VID `2dc8`) ou `2dc8:3106` no XInput,
com a regra udev de `11-xinput-xusb.md`.

## `8bdgui` — GTK4

```bash
cd native && make          # gera 8bdgui e xsusb
cd .. && ./run-gui.sh      # GSK_RENDERER=gl, layout adaptável a tiling
```

Abas: **Mapping**, **XInput (USB)**, **Sticks**, **Triggers**, **Vibration**,
**Macro** e **Perfil**. O painel esquerdo lista as abas e os limites do
protocolo; o rodapé tem o seletor de hidraw, `Procurar` e a barra de status.

Fluxo de gravação:

1. `Procurar` enumera os hidraw `2dc8`.
2. `Reset` (ou `Ctrl+R`) lê o record de 1652 B do controle.
3. Ligar o switch **Permitir gravar** na barra superior.
4. `Sync to device` (ou `Ctrl+G`) grava chunks + commit. `--write-selftest`
   faz o caminho completo e relê para comparar.

Atalhos: `Ctrl+R` ler, `Ctrl+G` gravar/sincronizar, `Ctrl+P` procurar de novo.
Na aba **XInput (USB)**, o app reivindica a interface do `xpad` enquanto estiver
aberto; ao fechar, devolve o controle ao driver do sistema.

Opções de linha de comando: `--auto-read` (lê 600 ms após abrir),
`--selftest` (lê e imprime o record), `--write-selftest ARQUIVO` (grava, relê e
compara ignorando `0x00C..0x00F` e `0x012`). Variáveis úteis: `BD_LAYOUT`
(`vertical`/`horizontal`), `BD_START_TAB` (nome da aba) e
`BD_NO_AUTO_XINPUT=1` (não conecta no XInput automaticamente).

Exemplo real (usa o arquivo gerado na seção `bd_build`):

```bash
cd native
./8bdgui --write-selftest /tmp/bd-base.bin
```

## `bd_cli` — list, dump, write e info

Interface única sobre `native/bd_hid.h` (`bd_hid_find`, `bd_hid_open`,
`bd_hid_execute`, `bd_hid_read_record`, `bd_hid_write_record`).

```bash
cd linux
./bd_cli list
./bd_cli dump ARQUIVO
./bd_cli info
./bd_cli write ARQUIVO [--verify] [--dry-run] [--no-commit] \
  [--first-chunk] [--pid N] [--device CAMINHO]
```

| comando | efeito |
|---|---|
| `list` | enumera os hidraw `2dc8` |
| `dump ARQUIVO` | lê o record de 1652 B e grava no arquivo |
| `info` | `cmd 0x0002`: lê 20 B de estado e mostra slot + se o PID usa CRC de header |
| `write ARQUIVO` | grava chunks de 45 B (`cmd=0x0001`, ACK ecoa `sub=1`) e commit `cmd=0x0006/subcmd=0x0015` (ACK ecoa `cmd=6`) |

`dump` e `info` usam o primeiro controle encontrado. Em `write`, `--device`
força o caminho e `--pid` força o PID (necessário se o caminho não estiver em
`/sys/class/hidraw`). `--verify` relê e compara ignorando os campos gerenciados
pelo device: `0x00C..0x00F` (`crc_value`, contador) e `0x012..0x013`
(`cur_slot`, que fica 4). `--dry-run` só mostra o resumo do arquivo;
`--no-commit` envia os chunks sem commit e `--first-chunk` para no primeiro.

Exemplo real:

```bash
$ cd linux
$ ./bd_cli list
/dev/hidraw0  2dc8:3012
$ ./bd_cli dump /tmp/perfil.bin
/tmp/perfil.bin: 1652 bytes
$ ./bd_cli write /tmp/bd-base.bin --dry-run
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
cd linux
./bd_build SAIDA.bin [--mode N] [--slot N] [--p1 ACTION] [--p2 ACTION]
```

`--mode`: `0`=Switch, `1`=DInput (padrão), `3`=XInput. `--slot` é `0..2`
(padrão `0`); P1/P2 são as entradas 18/19 e a sobrescrita vale só para o slot
escolhido. `--p1`/`--p2` aceitam nome (layout Xbox: `A`, `B`, `X`, `Y`, `LB`,
`RB`, `LT`, `RT`, `LS`, `RS`, `VIEW`, `MENU`, `HOME`, `STAR`, `BT_CON`,
`D_UP`, `D_DOWN`, `D_LEFT`, `D_RIGHT`, `SCREENSHOT`, `TURBO_SINGLE`,
`TURBO_AUTO`, `P1`, `P2`, `SWAP`) ou valor hex. Os nomes do vocabulário
8BitDo/Switch valem como alias: `L1`=`LB`, `R1`=`RB`, `L2`=`LT`, `R2`=`RT`,
`L3`=`LS`, `R3`=`RS`, `SELECT`=`VIEW`, `START`=`MENU`, `UP`/`DOWN`/`LEFT`/
`RIGHT`=`D_*`.

Atenção: no record, `RT` é o **gatilho direito** (0x8000); o d-pad → é
`D_RIGHT` (0x40). O índice 17 da tabela de chaves se chama `D_RIGHT` justamente
para não confundir os dois.

Exemplo real:

```bash
$ ./bd_build /tmp/bd-base.bin --mode 1 --slot 0 --p1 A --p2 B
gerado /tmp/bd-base.bin: 1652 bytes | modo=DInput slot=0 | P1=0x2000 P2=0x1000
```

## `bd_watch` — botões ao vivo

Mostra o estado dos botões A/B/X/Y do modo DInput (`0x3012`) lendo o report ID
3 do hidraw; o ID 2 (vendor) é descartado. O caminho é opcional e o padrão é
`/dev/hidraw0`. A linha só muda quando os bits de A/B/X/Y mudam; o restante do
report aparece em hex.

```bash
cd linux
./bd_watch /dev/hidraw0     # Ctrl+C para sair
```

## `xsusb`

Fala com o `2dc8:3106` no modo XInput (vendor class `0xFF/0x5D/0x01`), sem HID.
Ao abrir, desanexa o `xpad`, então o jogo deixa de ver o controle enquanto a
ferramenta estiver rodando; ao sair, o driver é reanexado. O rumble é zerado na
saída.

```bash
cd native
./xsusb <modo>
```

| modo | uso |
|---|---|
| `--watch` | estado ao vivo de botões, d-pad, sticks e gatilhos |
| `--rumble L R` | vibração, cada lado em `0..65535` (pulso de ~300 ms) |
| `--vendor-probe CMD [PARAM]` | envia `81 len 04 …` no EP `0x02` e escuta 1200 ms; padrão `CMD=0x10`, `PARAM=0` |
| `--listen MS` | só escuta o EP `0x81` por `MS` (padrão 1000 ms) e imprime os reports |

Exemplo real:

```bash
./xsusb --watch
```

## Nota comum de I/O

A gravação (`bd_cli write` e `8bdgui`) abre o hidraw com `O_NONBLOCK` e usa
`poll(POLLOUT)` antes de repetir `write()`: sem isso, o `write()` trava para
sempre quando o endpoint de saída do controle não é drenado. `bd_watch` também
usa `O_NONBLOCK`, mas apenas para leitura.
