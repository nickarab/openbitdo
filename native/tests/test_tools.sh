#!/bin/sh
# Teste offline de bd_build e bd_cli (sem abrir /dev/hidraw*).
#
# Usa os binários de BD_BIN_DIR (o run-tests.sh passa os que acabou de gerar
# com make). Sem BD_BIN_DIR, compila com o Makefile num diretório temporário.
# Build quebrado é FAIL: estes binários são os mesmos que o AppImage empacota.
set -u

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd) || exit 1
ROOT=$(CDPATH= cd "$SCRIPT_DIR/../.." && pwd) || exit 1
OUT=$(mktemp -d "${TMPDIR:-/tmp}/bd_tools_test.XXXXXX") || exit 1
trap 'rm -rf "$OUT"' EXIT HUP INT TERM

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; }
bad() {
    FAIL=$((FAIL + 1))
    printf 'FAIL  %s\n' "$1"
    [ -n "${2:-}" ] && printf '      %s\n' "$2"
    return 0
}

BIN=${BD_BIN_DIR:-}
if [ -z "$BIN" ]; then
    BIN="$OUT/build"
    if ! make -s -C "$ROOT" BUILD="$BIN" CFLAGS="-O2 -Werror" \
            "$BIN/bd_build" "$BIN/bd_cli" >"$OUT/make.log" 2>&1; then
        bad "bd_build/bd_cli não compilam (C23, -Werror)" "$(grep -m1 -E 'error|warning' "$OUT/make.log")"
        printf '%d pass, %d fail\n' "$PASS" "$FAIL"
        exit 1
    fi
fi
for tool in bd_build bd_cli; do
    if [ ! -x "$BIN/$tool" ]; then
        bad "$BIN/$tool ausente"
        printf '%d pass, %d fail\n' "$PASS" "$FAIL"
        exit 1
    fi
done
BD_BUILD="$BIN/bd_build"
BD_CLI="$BIN/bd_cli"

# validate <arquivo> <script python>: roda as asserções sobre o record.
validate() {
    python3 - "$1" 2>"$OUT/validate.err" >/dev/null
}

if "$BD_BUILD" "$OUT/default.bin" --p1 A --p2 B >"$OUT/build.out" 2>&1; then
    ok "bd_build gera um record"
else
    bad "bd_build falhou ao gerar o record" "$(tail -n 1 "$OUT/build.out")"
fi

if validate "$OUT/default.bin" <<'PY'
import struct
import sys

RECORD_LEN = 0x674
OFF_FLAG = 0x000
OFF_CRC = 0x00C
OFF_MODE = 0x010
OFF_NAME = 0x014
OFF_MAP = 0x0E0
OFF_MACRO = 0x1DC
NAME_SIZE = 32
MAP_SLOT_SIZE = 4 + 20 * 4
FEATURE_MASK = 0x20200911

record = open(sys.argv[1], "rb").read()
assert len(record) == RECORD_LEN, f"tamanho {len(record)} != {RECORD_LEN}"

# 0x000/0x004/0x008: feature mask (com 0xFFFFFFFF o firmware não aplica o perfil).
flags = struct.unpack_from("<3I", record, OFF_FLAG)
assert flags == (FEATURE_MASK,) * 3, f"flags={[hex(v) for v in flags]}"

# 0x1DC: sem o magic o firmware descarta o record inteiro.
macro_flag = struct.unpack_from("<I", record, OFF_MACRO)[0]
assert macro_flag == FEATURE_MASK, f"macro flag=0x{macro_flag:x}"

names = []
for slot in range(3):
    start = OFF_NAME + slot * NAME_SIZE
    names.append(record[start:start + NAME_SIZE].decode("utf-16-le").split("\0", 1)[0])
assert names == ["Profile 1", "Profile 2", "Profile 3"], f"nomes={names!r}"

mode, slot = struct.unpack_from("<HH", record, OFF_MODE)
assert (mode, slot) == (1, 0), f"modo/slot padrão={mode}/{slot}"

base = OFF_MAP + slot * MAP_SLOT_SIZE + 4
keys = struct.unpack_from("<20I", record, base)
assert keys[0] == 0x2000 and keys[1] == 0x1000, f"A/B=0x{keys[0]:x}/0x{keys[1]:x}"
assert (keys[18], keys[19]) == (0x2000, 0x1000), f"P1/P2=0x{keys[18]:x}/0x{keys[19]:x}"

# 0x00C é contador do device: a ferramenta não inventa o campo.
crc = struct.unpack_from("<I", record, OFF_CRC)[0]
assert crc == 0, f"crc_value=0x{crc:x}"
PY
then
    ok "record padrão: 1652 B, feature mask, macro flag, nomes, modo/slot, P1/P2, crc_value=0"
else
    bad "record padrão inválido" "$(tail -n 1 "$OUT/validate.err")"
fi

if "$BD_BUILD" "$OUT/custom.bin" --mode 3 --slot 2 --p1 A --p2 B >/dev/null 2>&1 &&
   validate "$OUT/custom.bin" <<'PY'
import struct
import sys

record = open(sys.argv[1], "rb").read()
mode, slot = struct.unpack_from("<HH", record, 0x010)
assert (mode, slot) == (3, 2), f"modo/slot={mode}/{slot}"
for s in range(3):
    base = 0x0E0 + s * (4 + 20 * 4) + 4
    p1, p2 = struct.unpack_from("<II", record, base + 18 * 4)
    want = (0x2000, 0x1000) if s == 2 else (0, 0)
    assert (p1, p2) == want, f"slot {s}: P1/P2=0x{p1:x}/0x{p2:x}"
PY
then
    ok "bd_build --mode 3 --slot 2: P1/P2 só no slot 2"
else
    bad "bd_build não respeita --mode/--slot" "$(tail -n 1 "$OUT/validate.err" 2>/dev/null)"
fi

# --from: patch de um slot só; os outros dois ficam como no arquivo base.
python3 - "$OUT/base.bin" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytes((i * 7 + 3) & 0xFF for i in range(0x674)))
PY
if "$BD_BUILD" "$OUT/patched.bin" --from "$OUT/base.bin" --slot 1 --p1 X >/dev/null 2>&1 &&
   python3 - "$OUT/base.bin" "$OUT/patched.bin" 2>"$OUT/validate.err" <<'PY'
import struct
import sys

base = open(sys.argv[1], "rb").read()
out = open(sys.argv[2], "rb").read()
assert len(out) == 0x674

def slot_ranges(slot):
    yield 0x000 + slot * 4, 4                       # flag
    yield 0x014 + slot * 32, 32                     # nome
    yield 0x074 + slot * 12, 4                      # flag do rumble
    yield 0x098 + slot * 8, 8                       # sticks
    yield 0x0B0 + slot * 8, 8                       # gatilhos
    yield 0x0C8 + slot * 8, 4                       # flag de feature
    yield 0x0E0 + slot * 84, 84                     # mapa

touched = set(range(0x010, 0x014)) | set(range(0x1DC, 0x1E0))
for start, size in slot_ranges(1):
    touched |= set(range(start, start + size))
changed = [i for i in range(0x674) if base[i] != out[i] and i not in touched]
assert not changed, f"bytes fora do slot 1 alterados: primeiro em 0x{changed[0]:03x}"

p1 = struct.unpack_from("<I", out, 0x0E0 + 84 + 4 + 18 * 4)[0]
assert p1 == 0x10, f"P1 do slot 1 = 0x{p1:x}"
PY
then
    ok "bd_build --from preserva os slots não editados"
else
    bad "bd_build --from alterou outros slots" "$(tail -n 1 "$OUT/validate.err")"
fi

reject() {
    if "$BD_BUILD" "$OUT/reject.bin" "$@" >/dev/null 2>&1; then
        bad "bd_build aceitou $*"
    else
        ok "bd_build rejeita $*"
    fi
}
reject --p1 NAOEXISTE
reject --mode 4
reject --slot 3
reject --mode abc
reject --slot
reject --opcao-que-nao-existe 1
reject --from "$OUT/nao-existe.bin"

"$BD_CLI" >"$OUT/cli_usage.out" 2>&1
usage_status=$?
if [ "$usage_status" -eq 2 ] && grep -qiE 'uso|usage' "$OUT/cli_usage.out"; then
    ok "bd_cli sem argumentos imprime uso (exit 2)"
else
    bad "bd_cli sem argumentos: exit $usage_status (esperado 2 com uso)"
fi

"$BD_CLI" subcomando-inexistente >/dev/null 2>&1
if [ $? -eq 2 ]; then
    ok "bd_cli rejeita subcomando desconhecido (exit 2)"
else
    bad "bd_cli aceitou subcomando desconhecido"
fi

"$BD_CLI" mode 9 >/dev/null 2>&1
if [ $? -eq 2 ]; then
    ok "bd_cli mode 9 rejeitado antes de tocar no device (exit 2)"
else
    bad "bd_cli mode 9 não foi rejeitado com exit 2"
fi

# Argumentos inválidos são recusados antes de procurar o device (exit 2).
cli_rejects() {
    "$BD_CLI" "$@" >/dev/null 2>&1
    if [ $? -eq 2 ]; then
        ok "bd_cli rejeita: $*"
    else
        bad "bd_cli aceitou: $*"
    fi
}
cli_rejects patch abc
cli_rejects patch 3
cli_rejects patch -1
cli_rejects patch 0 --mode 9
cli_rejects patch 0 --mode x
cli_rejects patch 0 --p1 NAOEXISTE
cli_rejects patch 0 --p1
cli_rejects patch 0 --opcao 1
cli_rejects send-a 256 0
cli_rejects send-a 0x09 abc
cli_rejects mode 1x

if "$BD_CLI" write "$OUT/default.bin" --dry-run >"$OUT/dry.out" 2>&1 &&
   grep -q 'dry-run' "$OUT/dry.out"; then
    ok "bd_cli write --dry-run lê o arquivo sem enviar nada"
else
    bad "bd_cli write --dry-run falhou" "$(tail -n 1 "$OUT/dry.out")"
fi

head -c 100 "$OUT/default.bin" >"$OUT/short.bin"
if "$BD_CLI" write "$OUT/short.bin" --dry-run >/dev/null 2>&1; then
    bad "bd_cli write aceitou record curto"
else
    ok "bd_cli write rejeita record com tamanho errado"
fi

if ! command -v timeout >/dev/null 2>&1; then
    bad "timeout indisponível para testar bd_cli list"
else
    timeout 5 "$BD_CLI" list >/dev/null 2>&1
    list_status=$?
    if [ "$list_status" -eq 124 ]; then
        bad "bd_cli list travou por mais de 5 s"
    else
        ok "bd_cli list retorna sem travar (exit $list_status)"
    fi
fi

printf '%d pass, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
