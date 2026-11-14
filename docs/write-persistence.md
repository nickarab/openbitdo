# 14 — Gravação real no controle: o que o hardware provou

Este documento corrige suposições anteriores e registra o fluxo que **funciona**.

## Fluxo verificado (DInput, `2dc8:3012`, HID)

```
1. read    Type C  cmd=0x0002, total=0x674, offsets crescentes (45 B)
2. write   Type C  cmd=0x0001 por chunk de 45 B  → device responde ecoando sub=1
3. commit  Type B  cmd=0x0006, subcmd=0x0015     → device responde ecoando cmd=6
4. read    comparação
```

Resultado medido (`8bdgui --write-selftest`, e `8bdctl dump` depois):

```
escrita: ACK (chunks + commit)
releitura: 0 byte(s) divergente(s) fora dos campos do device
```

## Três descobertas que contradizem a leitura inicial do binário

| campo | suposição inicial | comportamento real |
|---|---|---|
| `crc_value` (0x00C) | CRC-16 do record, calculado pelo app | **contador gerenciado pelo device**: enviamos `0x7f6b`, ele gravou `1`; no commit seguinte, `2`. O app oficial calcula esse campo para exibição/undo, mas o device o reescreve. |
| `cur_slot` (0x012) | slot ativo (0..2) | permanece **4** (sentinela "sem slot") mesmo após `cmd 0x09` (`changeUSBUltimate`). |
| header CRC (+6) | obrigatório | só existe para PIDs da lista; `0x3012` está fora ⇒ header sem CRC. |

⇒ Não era o CRC que bloqueava a gravação.

## A causa real do travamento

`write()` no hidraw **bloqueia para sempre** quando o device para de consumir pacotes
(o out endpoint não é drenado). Sintoma: a ferramenta ficava pendurada sem mensagem.

Correção (aplicada em `8bdgui.c` e `8bdwrite.c`):

```c
open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
ssize_t n = write(fd, frame, sizeof frame);
if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    poll(POLLOUT, 100 ms) e repete (até 20×);
```

## Estado no app (`8bdgui`)

- `Ler perfil` → 1652 B; `Permitir gravar` (switch) + `Sync to device` → grava.
- `write_record` agora **não inventa** o campo de CRC e **valida o ACK do commit**.
- `--write-selftest <arquivo.bin>` prova o caminho completo sem UI (usado nos testes).
- Erros viram mensagem na barra de status (sem congelar: I/O em `GThread`).

## Pendente de prova física

1. **Efeito**: P1/P2 remapeados (gravamos `P1=0x2000` = A, `P2=0x1000` = B) precisam
   aparecer como A/B ao apertar. Medição: `/dev/hidraw0`, report ID 3 — em DInput o mesmo
   hidraw também transporta os reports vendor (id 2), então o monitor filtra por id 3.
2. **Persistência**: desligar/religar o controle e reler; se `0x0E0+4+18*4 = 0x2000`
   sobreviver, está em flash.
