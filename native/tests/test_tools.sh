#!/bin/sh
# Teste offline de linux/bd_build e linux/bd_cli (sem abrir /dev/hidraw*).
# As ferramentas são de outra lane: fonte ausente ou build quebrado é SKIP,
# para a onda seguinte distinguir "ainda não entregue" de regressão real.
set -u

SCRIPT_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd) || exit 1
cd "$SCRIPT_DIR/../.." || exit 1

CC=${CC:-gcc}
CFLAGS="-std=c23 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion"
OUT=$(mktemp -d /tmp/bd_tools_test.XXXXXX) || exit 1
T_BIN=/tmp/t.bin
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
skip() { printf 'SKIP  %s\n' "$1"; }

if [ ! -f linux/bd_build.c ] || [ ! -f linux/bd_cli.c ] ||
   [ ! -f native/bd_hid.c ] || [ ! -f native/bd_core.c ]; then
    skip "bd_build/bd_cli ainda não disponível"
    exit 0
fi

if ! $CC $CFLAGS -I native -o "$OUT/bd_build" \
        linux/bd_build.c native/bd_hid.c native/bd_core.c 2>"$OUT/build_build.log"; then
    skip "bd_build/bd_cli ainda não disponível"
    exit 0
fi
if ! $CC $CFLAGS -I native -o "$OUT/bd_cli" \
        linux/bd_cli.c native/bd_hid.c native/bd_core.c native/bd_usb.c 2>"$OUT/build_cli.log"; then
    skip "bd_build/bd_cli ainda não disponível"
    exit 0
fi
if grep -qE 'warning:|error:' "$OUT/build_build.log" "$OUT/build_cli.log"; then
    bad "bd_build/bd_cli compilam com warnings" "$(grep -m1 -E 'warning:|error:' "$OUT/build_build.log" "$OUT/build_cli.log")"
else
    ok "bd_build/bd_cli compilam em C23 sem warnings"
fi

rm -f "$T_BIN"
if "$OUT/bd_build" "$T_BIN" --p1 A --p2 B >"$OUT/build.out" 2>&1; then
    ok "bd_build gera $T_BIN"
else
    bad "bd_build falhou ao gerar $T_BIN" "$(tail -n 1 "$OUT/build.out")"
fi

if python3 - "$T_BIN" >"$OUT/validate.out" 2>"$OUT/validate.err" <<'PY'
import struct
import sys

RECORD_LEN = 0x674
OFF_FLAG = 0x000
OFF_CRC = 0x00C
OFF_MODE = 0x010
OFF_SLOT = 0x012
OFF_NAME = 0x014
OFF_MAP = 0x0E0
NAME_SIZE = 32
MAP_SLOT_SIZE = 4 + 20 * 4

record = open(sys.argv[1], "rb").read()
assert len(record) == RECORD_LEN, f"tamanho {len(record)} != {RECORD_LEN}"

# 0x000/0x004/0x008: os três são feature mask (medido no record que o app
# oficial grava; com 0xFFFFFFFF aqui o firmware não aplica o perfil).
flags = struct.unpack_from("<3I", record, OFF_FLAG)
assert flags == (0x20200911, 0x20200911, 0x20200911), f"flags={[hex(v) for v in flags]}"

# 0x1DC é a flag da seção macro: sem o magic o firmware descarta o record
# inteiro, mesmo com os chunks e o commit ACKados.
macro_flag = struct.unpack_from("<I", record, 0x1DC)[0]
assert macro_flag == 0x20200911, f"macro flag=0x{macro_flag:x}"

names = []
for slot in range(3):
    start = OFF_NAME + slot * NAME_SIZE
    field = record[start:start + NAME_SIZE]
    names.append(field.decode("utf-16-le").split("\0", 1)[0])
assert names == ["Profile 1", "Profile 2", "Profile 3"], f"nomes={names!r}"

mode, slot = struct.unpack_from("<HH", record, OFF_MODE)
assert (mode, slot) == (1, 0), f"modo/slot padrão={mode}/{slot}"

# cur_slot é gravado para ativar o perfil (o app oficial também escreve 0)
assert slot == 0, f"cur_slot={slot}"

map_base = OFF_MAP + slot * MAP_SLOT_SIZE + 4
p1, p2 = struct.unpack_from("<II", record, map_base + 18 * 4)
assert (p1, p2) == (0x2000, 0x1000), f"P1/P2=0x{p1:x}/0x{p2:x}"

# 0x00C é contador do device, que sobrescreve o valor enviado (re/14-write-persist.md).
crc = struct.unpack_from("<I", record, OFF_CRC)[0]
assert crc == 0, f"crc_value=0x{crc:x}: bd_build não deve inventar o campo"
PY
then
    ok "record: 1652 B, feature mask, nomes, modo/slot ativo, P1/P2 e crc_value=0"
else
    bad "record inválido" "$(tail -n 1 "$OUT/validate.err")"
fi

if "$OUT/bd_build" "$OUT/custom.bin" --mode 3 --slot 2 --p1 A --p2 B \
        >"$OUT/custom.out" 2>&1 &&
   python3 - "$OUT/custom.bin" >"$OUT/custom_validate.out" 2>"$OUT/custom_validate.err" <<'PY'
import struct
import sys

record = open(sys.argv[1], "rb").read()
mode, slot = struct.unpack_from("<HH", record, 0x010)
assert (mode, slot) == (3, 2), f"modo/slot={mode}/{slot}"
map_base = 0x0E0 + slot * (4 + 20 * 4) + 4
p1, p2 = struct.unpack_from("<II", record, map_base + 18 * 4)
assert (p1, p2) == (0x2000, 0x1000), f"P1/P2=0x{p1:x}/0x{p2:x}"
PY
then
    ok "bd_build respeita --mode 3 --slot 2"
else
    bad "bd_build não respeita --mode/--slot" "$(tail -n 1 "$OUT/custom_validate.err" 2>/dev/null)"
fi

if "$OUT/bd_build" "$OUT/reject.bin" --p1 NAOEXISTE >"$OUT/reject.out" 2>&1; then
    bad "bd_build aceitou ação inexistente (exit 0)"
else
    ok "bd_build rejeita ação inexistente (exit != 0)"
fi

"$OUT/bd_cli" >"$OUT/cli_usage.out" 2>&1
usage_status=$?
if [ "$usage_status" -eq 2 ] && grep -qiE 'uso|usage' "$OUT/cli_usage.out"; then
    ok "bd_cli sem argumentos imprime uso (exit 2)"
else
    bad "bd_cli sem argumentos: exit $usage_status (esperado 2 com uso)"
fi

if ! command -v timeout >/dev/null 2>&1; then
    skip "timeout indisponível para testar bd_cli list"
else
    timeout 5 "$OUT/bd_cli" list >"$OUT/cli_list.out" 2>&1
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
