/*
 * bd_build — monta e grava em arquivo o record de perfil de 1652 B.
 * Layout: ../re/03-profile.md §5. O arquivo é o payload cru de
 * bd_hid_write_record: header Tipo C e chunking pertencem ao transporte.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bd_core.h"

static constexpr uint32_t CONFIG_ENABLED = 1;
static constexpr uint8_t RANGE_MIN = 0;
static constexpr uint8_t RANGE_MAX = 255;
static constexpr size_t RUMBLE_SLOT_SIZE = 12;            /* flag + zoom_out[2] */
static constexpr size_t RANGE_SLOT_SIZE = 8;              /* flag + range[2] */
static constexpr size_t FEATURE_SLOT_SIZE = 8;            /* flag + featrue */
static constexpr size_t MAP_SLOT_SIZE = 4 + BD_NKEYS * 4; /* flag + key[20] */
static constexpr size_t MODE_COUNT = sizeof bd_mode_names / sizeof bd_mode_names[0];

/*
 * key[] na ordem MAP_KEY_E, cada tecla mapeada para a própria ação.
 * STAR (12) usa KEY_MENU_MAP 0x10000 e HOME (13) usa 0x8000
 * (09-native-gui.md); P1/P2 (18/19) não emitem nada por padrão.
 */
static const uint32_t identity_map[BD_NKEYS] = {
    0x2000, 0x1000, 0x10, 0x20,     /* A B X Y */
    0x400, 0x800, 0x4000, 0x20000,  /* L1 R1 L2 R2 */
    0x1, 0x2, 0x4, 0x8,             /* L3 R3 SELECT START */
    0x10000, 0x8000,                /* STAR HOME */
    0x200, 0x100, 0x80, 0x40,       /* UP DOWN LEFT RIGHT */
    0x0, 0x0,                       /* P1 P2 */
};

static void set_full_range(uint8_t *record, size_t base)
{
    for (size_t slot = 0; slot < BD_SLOTS; ++slot) {
        const size_t at = base + slot * RANGE_SLOT_SIZE;
        bd_wr32(record, at, CONFIG_ENABLED);
        /* range[0] = eixo esquerdo, range[1] = direito; cada par é início/fim */
        record[at + 4] = RANGE_MIN;
        record[at + 5] = RANGE_MAX;
        record[at + 6] = RANGE_MIN;
        record[at + 7] = RANGE_MAX;
    }
}

static bool parse_number(const char *text, unsigned long *out)
{
    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value > UINT32_MAX)
        return false;
    *out = value;
    return true;
}

static bool parse_action(const char *text, uint32_t *out)
{
    unsigned long numeric = 0;
    if (parse_number(text, &numeric)) {
        *out = (uint32_t)numeric;
        return true;
    }
    for (size_t index = 0; index < bd_actions_count; ++index) {
        if (strcasecmp(bd_actions[index].name, text) == 0) {
            *out = bd_actions[index].value;
            return true;
        }
    }
    return false;
}

static int usage(const char *program)
{
    fprintf(stderr,
            "uso: %s <saida.bin> [--mode N] [--slot N] [--p1 ACAO] [--p2 ACAO]\n"
            "  modo: 0..3 (0=Switch, 1=DInput, 3=XInput) | slot: 0..%d\n"
            "  ACAO: nome (A,B,X,Y,L1,R1,L2,R2,L3,R3,SELECT,START,HOME,MENU,\n"
            "  UP,DOWN,LEFT,RIGHT,SCREENSHOT,TURBO_SINGLE,TURBO_AUTO,P1,P2,SWAP,\n"
            "  BT_CON) ou valor hex (0x2000)\n",
            program, BD_SLOTS - 1);
    return 2;
}

static int invalid_value(const char *option, const char *text)
{
    fprintf(stderr, "valor inválido para %s: %s\n", option, text);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage(argv[0]);

    const char *const output = argv[1];
    unsigned long mode = 1; /* DInput */
    unsigned long slot = 0;
    uint32_t p1 = 0;
    uint32_t p2 = 0;

    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "falta valor para %s\n", argv[i]);
            return 2;
        }
        const char *const option = argv[i];
        const char *const text = argv[i + 1];

        if (strcmp(option, "--mode") == 0) {
            if (!parse_number(text, &mode) || mode >= MODE_COUNT)
                return invalid_value(option, text);
        } else if (strcmp(option, "--slot") == 0) {
            if (!parse_number(text, &slot) || slot >= BD_SLOTS)
                return invalid_value(option, text);
        } else if (strcmp(option, "--p1") == 0) {
            if (!parse_action(text, &p1))
                return invalid_value(option, text);
        } else if (strcmp(option, "--p2") == 0) {
            if (!parse_action(text, &p2))
                return invalid_value(option, text);
        } else {
            fprintf(stderr, "opção desconhecida: %s\n", option);
            return 2;
        }
    }

    uint8_t record[BD_REC_LEN];
    /* 0x00C (crc_value) fica zerado: é contador do device, que sobrescreve o
     * valor enviado. Não é CRC do record (14-write-persist.md). */
    memset(record, 0, sizeof record);

    bd_wr16(record, BD_OFF_MODE, (uint16_t)mode);
    /* 0x012 é cur_slot: o device mantém o próprio valor (4 = "sem slot") após
     * o commit, então gravar aqui só marca o slot que este arquivo edita. */
    bd_wr16(record, BD_OFF_SLOT, (uint16_t)slot);

    for (size_t s = 0; s < BD_SLOTS; ++s) {
        bd_wr32(record, BD_OFF_FLAG + s * 4, CONFIG_ENABLED);
        bd_wr32(record, BD_OFF_RUM + s * RUMBLE_SLOT_SIZE, CONFIG_ENABLED);
        bd_wr32(record, BD_OFF_FEAT + s * FEATURE_SLOT_SIZE, CONFIG_ENABLED);

        char name[BD_NAME_UNITS];
        snprintf(name, sizeof name, "Profile %zu", s + 1);
        bd_name_set(record, (int)s, name);
    }

    set_full_range(record, BD_OFF_JOY);
    set_full_range(record, BD_OFF_TRG);

    for (size_t s = 0; s < BD_SLOTS; ++s) {
        const size_t at = BD_OFF_MAP + s * MAP_SLOT_SIZE;
        bd_wr32(record, at, CONFIG_ENABLED);
        for (size_t key = 0; key < BD_NKEYS; ++key)
            bd_wr32(record, at + 4 + key * 4, identity_map[key]);
    }

    /* P1/P2 são as entradas 18/19 de key[]; sobrescrever só no slot editado. */
    const size_t map_slot = BD_OFF_MAP + (size_t)slot * MAP_SLOT_SIZE + 4;
    bd_wr32(record, map_slot + (BD_NKEYS - 2) * 4, p1);
    bd_wr32(record, map_slot + (BD_NKEYS - 1) * 4, p2);

    FILE *file = fopen(output, "wb");
    if (file == nullptr) {
        perror("fopen");
        return 1;
    }
    const size_t written = fwrite(record, 1, sizeof record, file);
    const int close_status = fclose(file);
    if (written != sizeof record || close_status != 0) {
        fprintf(stderr, "falha ao gravar %s\n", output);
        return 1;
    }

    printf("gerado %s: %zu bytes | modo=%s slot=%lu | P1=0x%x P2=0x%x\n",
           output, sizeof record, bd_mode_names[mode], slot, p1, p2);
    return 0;
}
