# 16 — Protocolo HID vendor do Ultimate (canônico)

Consolida 03, 05, 06, 07, 11 e 14; os originais ficam como registro de bancada. Nenhuma
captura nova: nada aqui exigiu abrir `/dev/hidraw*`, rodar Wine ou tocar em `../app/`.

## 1. Canal

| item | valor |
|---|---|
| Usage Page / VID | `0xFFA0` (vendor) / `0x2DC8` |
| Report IDs | `0x02` entrada e `0x81` saída, **63 B** de dados cada |
| Quadro no fio | **64 B** fixos = 1 B de Report ID + 63 B de dados; sobra à direita é zero |
| Onde existe | dongle `2dc8:3109` e controle pareado ao dongle/BT. No cabo `2dc8:3106` a interface é vendor `ff/5d/01` entregue ao `xpad`: **não há HID**, logo não há perfil por este caminho (11) |

## 2. Framing

| campo | SAÍDA (host → device) | ENTRADA (device → host) |
|---|---|---|
| Report ID | `+0` = `0x81` | `+0` = `0x02` |
| tag ADVANCE | `+2` = `0x04` | `+1` = `0x04` |
| contador | `+1` = `N + 17` (tag + header + payload) | — |
| header | `+3`, 16 B | `+2`, 16 B |
| payload | `+19`, `N` B | `+18` |

* `N ≤ 45` porque `64 − (ID + contador + tag + header) = 64 − 19`; com `N = 45` o quadro é
  exatamente 64 B. `FUN_103c6d60` ainda limita `header+payload` a 61 (`0x3D`) e
  `FUN_103c60e0` corta o payload Tipo C em 45 (`0x2D`) — o mesmo limite por dois caminhos.
* Com o descriptor de 63 B, o quadro de 64 B é o report inteiro: não existe byte extra
  além do report.
* Requisição de leitura (sem payload) e resposta, capturadas em 07 §2:

```
TX 81 11 04 | 02 00 | 00 00 | 14 00 | ff ff | 14 00 00 00 | 00 00 00 00
RX 02 04    | 04 00 | 02 00 | 14 00 | ff ff | 14 00 00 00 | 00 00 00 00 | 20 B de dados
```

O header da resposta começa em `+2`; o primeiro u16 é `0x0004` (ACK) e o segundo é o eco do
`cmd` pedido. É o par `total`/`offset` (+8/+12) que prova o alinhamento — 07 §2 marcou o
início em `+3` e por isso leu `cmd` como eco.

## 3. Header (16 B, little-endian)

| Tipo | layout | ACK esperado | CRC | usado por |
|---|---|---|---|---|
| A | `+0 u8 cmd`; `+1..3` zero; `+4 u32 param`; `+8..15` zero | `+0` = `0x04`; `+4..7` = eco do cmd (u32) | não | `0x09`, `0x0F`, `0x10` USB |
| B | `+0 u16 cmd`; `+2 u16 subcmd`; `+4..15` zero | `+0` = `0x0004`; `+2` = eco do cmd (u16) | não | `0x0006/0x0015`, `0x10` BT |
| C | `+0 u16 cmd`; `+2 u16 subcmd`; `+4 u16 len`; `+6 u16 crc`; `+8 u32 total`; `+12 u32 offset` | `+0` = `0x0004`; `+2` = eco do cmd pedido; `+4` = bytes devolvidos | só PIDs da lista | `0x0002/0x0000`, `0x0001/0x0000` |

No Tipo C, `total` é o tamanho da transferência inteira e `offset` a posição do chunk; o
host avança `offset` pelo `len` **devolvido** no ACK (`+4`), não pelo tamanho pedido
(03 §4.1/§4.8). O `+6` da resposta é descartado (03 §8).

## 4. Comandos

| cmd / subcmd | Tipo | pedido | ACK a esperar | uso |
|---|---|---|---|---|
| `0x0002` / `0x0000` | C | `len ≤ 45`, `total`, `offset`, sem payload | `+2` = `2` | ler record (`total 0x674`) ou janela de 20 B (`total 0x14`) |
| `0x0001` / `0x0000` | C | chunk de até 45 B, `total 0x674`, `offset` | `+2` = `1` | gravar record |
| `0x0006` / `0x0015` | B | — | `+2` = `6` | commit da escrita |
| `0x09` | A | `u32` índice | **não lido** | trocar slot/índice |
| `0x0F` | A | `u32` plataforma | `+4` = `0x0F` | plataforma, transporte USB |
| `0x0010` / `plataforma` | B | `u16` plataforma | `+2` = `0x10` | plataforma, transporte BT |
| `0x10` | A | — | 6 B de dados | status no USB (`info`: vid/pid/tipo u16 BE, não capturado) |
| `0x14` | — | — | 6 B de dados | status no BT (05 §5) |

* `0x10` tem dois papéis: status no USB (Tipo A) e plataforma no BT (Tipo B). O ramo é
  escolhido por `DAT_10bc2168`, que só é 1 nos PIDs `0x3105`/`0x3107`; no `0x3109` o
  comando de plataforma é `0x0F` (03 §3.1, 06 §3).
* Plataforma: `0` Switch, `1` Windows, `2` Mac, `3` Android, `4` None (enum
  `USB_GAMEPAD_MODE`, 03 §7.2).
* `0x0A` (`setChangePlat`) e `0x11`/`0x51` (disconnect) existem em 03 §3.1 e ficam fora do
  canal de perfil.

## 5. CRC

| uso | rotina | variante | init | pós-processamento | cobertura |
|---|---|---|---|---|---|
| header Tipo C, `+6` | `0x103c75b0` | ARC refletido `0xA001` | `0xFFFF` | nenhum | payload do chunk |
| record (`crc_value`) e exports `_ANSI_CRC_16_*` | `0x103c5960` | CCITT refletido `0x8408` (`0x1021` com bits invertidos) | `0xFFFF` | troca os dois bytes | 1652 B (`0x674`) |

| PID | modelo / modo | CRC no header |
|---|---|---|
| `0x3011` | Ultimate_PC (03 §3.2) | sim |
| `0x3012` | Ultimate 2.4G / DInput (07 §1, 14) | **não** |
| `0x6007` | UltimateBT (03 §3.2) | sim |
| `0x3109` | dongle 2.4G "IDLE" (07 §1) | sim |
| `0x6006`, `0x3010`, `0x9023`, `0x9024` | não mapeados nos docs | sim |
| `0x3106` | XInput no cabo (11) | não tem HID |

* Fora da lista, `+4..+7` é um `u32 len` só, sem CRC; `0x3012` é o caso validado em 14.
* CRC de payload vazio = `0xFFFF`, capturado no fio (07 §2); vetor `123456789` → `0x916F`
  (12).
* O `+6` da resposta **não** é CRC dos dados devolvidos: o payload `ff`×16 + `03 00 04 00`
  daria `0x96E6` (`0x8408` + swap) ou `0x8432` (ARC) e o campo veio `0xFFFF` (07 §6). Não
  validar esse campo.
* 06 §2 corrigiu a lane 3 que chamava o CRC do **record** de ARC; o CRC do **header** é
  outra rotina (`0x103c75b0`, ARC em 03) e não foi revalidado em hardware. A implementação
  atual (`native/bd_hid.c`) usa `bd_crc16` (`0x8408` + swap) também no header; as duas
  variantes só divergem com payload não vazio, então um teste em PID da lista (ex. `0x3011`)
  decide — até lá o valor do header não é fato provado.

## 6. Record de 1652 B e a máquina de transferência

| offset | tam | campo | no protocolo |
|---|---|---|---|
| `0x000` | `0x674` | record inteiro | `total` de toda transferência |
| `0x00C` | 4 | `crc_value` | contador do device — §7 |
| `0x010` | 2 | `gamepad_mode` | 0 Switch / 1 DInput / 3 XInput (2 nunca visto) |
| `0x012` | 2 | `cur_slot` | 4 = "sem slot"; `0x0A` só como retorno de falha dos getters BT/Pro2 |

Mapa completo dos sub-registros: 03 §5.

1. `read` Tipo C `2/0`, `total 0x674`, `offset` crescente → 37 chunks (36 × 45 + 32).
2. `write` Tipo C `1/0` com os mesmos chunks; cada ACK ecoa `+2 = 1`.
3. `commit` Tipo B `6/0x0015`; o ACK ecoa `+2 = 6` — sem commit nada persiste.
4. reler e comparar: só `crc_value` e `cur_slot` divergem (campos do device).

* Slot: `read` Tipo C com `total 0x14`, `offset 0` → 20 B de payload e `cur_slot` em
  `payload + 0x12` (03 §4.5, 07 §2).
* Retry é por chunk: o app original zera o contador a cada ACK e desiste em 30 falhas
  consecutivas; o getter de slot do BT tenta 5× e vale a última (03 §4.5/§8).

## 7. Fatos de hardware provados (não "corrigir")

| fato | consequência |
|---|---|
| `write()` no hidraw **trava para sempre** quando o device para de drenar o endpoint de saída | `O_NONBLOCK` + `poll(POLLOUT, 100 ms)` com retry (14) |
| escrita = chunks de 45 B `cmd=0x0001`, cada um com ACK (device ecoa `+2 = 1`) | não avançar `offset` sem ACK |
| commit `cmd=0x0006` `subcmd=0x0015` com ACK (device ecoa `+2 = 6`) | commit é obrigatório para persistir |
| `crc_value` (`0x00C`) é **contador do device**: enviamos `0x7f6b`, ele gravou `1`; no commit seguinte, `2` | não calcular, não comparar, não "consertar" |
| `cur_slot` (`0x012`) fica **4** no device mesmo depois de `cmd 0x09` | não forçar 0 |
| CRC do header (`+6`) só nos PIDs de `bd_pid_uses_crc` (`native/bd_core.c`) | fora da lista o campo é `u32 len` |
| gravação validada em `2dc8:3012` (DInput), releitura com 0 divergência fora dos campos do device | é o caso de referência |
| dongle IDLE `0x3109`: Type C responde, Type A (`info`) não; record = 1648 × `ff` + `03 00 04 00` em `0x10` | resposta stub = controle não pareado, não falha de framing (07 §6) |

## 8.1 Ativação do perfil (o que faltava)

Escrita e commit ACKados **não bastam**: o firmware grava os chunks na flash e
descarta o record na hora de aplicar se alguma seção estiver sem o magic. Medido
comparando o record que o app oficial deixa no device (`2dc8:3012`) com o gerado
pelo `bd_build` — a única diferença estrutural era 1 word:

| offset | campo | valor exigido | sintoma se errado |
|---|---|---|---|
| `0x000`, `0x004`, `0x008` | feature mask ×3 | `0x20200911` | perfil não aplica |
| `0x0E0 + slot*84` | flag do `map_key` do slot | `0x20200911` | slot não aparece no controle |
| `0x1DC` | **flag da seção macro** | `0x20200911` | chunks+commit ACKados, record descartado |
| `0x012` | `cur_slot` | slot alvo (`0`..`2`) | device mantém `4` (nenhum) |
| `0x00C` | contador | qualquer | device sobrescreve (próprio contador) |

Com os cinco certos o remap é aplicado no firmware e sobrevive a reboot;
confirmado no hardware com P1→A (B0) e P2→B (B1) saindo do controle.

## 8.2 Modo do gamepad e disponibilidade do HID

`0x010` (mode: 0 Switch, 1 DInput, 3 XInput) decide como o controle enumera no
próximo boot:

| mode | PID | interface | HID vendor |
|---|---|---|---|
| 3 | `2dc8:3106` | `ff/5d/01` (xpad) | **não existe** |
| 1 | `2dc8:3012` | HID 63 B, reports `0x02`/`0x81` | sim |

Ou seja: em XInput não há canal de configuração — nem para o app oficial.

**Mas o modo é trocável por software.** O firmware aceita o mesmo frame de troca
(`81 05 00 51 <m>`) no **endpoint vendor `0x02`**, o mesmo que o xpad usa para
rumble: 32 bytes fazem o controle re-enumerar como `3012` (com hidraw) e `51 02`
o devolve a XInput. Medido no hardware, rodando como usuário comum.

| estado | caminho | comando |
|---|---|---|
| `3106` | usbfs: `DISCONNECT_CLAIM` + `SUBMITURB` no EP `0x02` | `bd_cli hid` (ou o app, automático) |
| `3012` | hidraw, report `0x81` | `bd_cli mode <m>` |

O knock de rumble que o app oficial usa (`HomeController.AutoDinput` →
`Xinput.start` → `subXinput_C`, 7 passos de vibração × 3) **não** troca o modo
neste firmware: a sequência foi reproduzida byte a byte (`00 08 00 01 00` …) e
o controle continuou em `3106`. O caminho que funciona é o frame no EP vendor.

## 8. O que NÃO é validado

| lacuna | evidência |
|---|---|
| bytes `+3..+5` e `+11..+17` da resposta (header `+1..3` e `+9..15`) | 05 §8, 06 §5 |
| posição do `len` na resposta: header `+4` (byte 6) em 03 §2.1 vs byte `+10` em 05 §3 | a captura de 07 (len 20 = total 20) não distingue |
| `+8`/`+12` da resposta são `total`/`offset` ecoados | presumível, nunca checado (03 §9.5) |
| CRC do header com payload ≠ vazio | só `0xFFFF` (payload vazio) foi visto no fio |
| semântica do `0x09` (índice de slot ou modo) | não há ACK nem leitura de volta (03 §9.1) |
| `0x3012` fora da lista de CRC: firmware ignora, ou o app troca de PID antes de ler | 03 §9.3 |
| efeito e persistência: P1/P2 remapeados como A/B, e se sobrevivem a desligar/religar | 14 §"Pendente de prova física" |
| timeout de 30 ms | é do app Windows (I/O sobreposto), não medido no hidraw (03 §9.6) |

## 9. Rastreabilidade

| doc | o que veio para cá |
|---|---|
| 03 | header A/B/C, tabela de opcodes, CRC do header, offsets do record |
| 05 | descriptor, framing de saída, `FUN_103c6d60`/`FUN_103c6640` |
| 06 | CRC do record (`0x8408` + swap) e a correção do "ARC" |
| 07 | captura viva do `read` Tipo C e o stub do dongle IDLE |
| 11 | `2dc8:3106` não tem HID (XUSB) |
| 14 | ACK de escrita/commit, contador `crc_value`, `cur_slot` 4, `O_NONBLOCK` |
