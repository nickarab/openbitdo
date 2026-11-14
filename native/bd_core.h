/* Constantes de protocolo e layout do record; origem na RE em ../re/. */
#ifndef BD_CORE_H
#define BD_CORE_H

#include <stddef.h>
#include <stdint.h>

enum bd_usb_id {
    BD_VID = 0x2DC8,
    BD_PID_DONGLE = 0x3109,   /* "8BitDo IDLE": canal HID 0xFFA0 */
};

/* Envelope montado por FUN_103c6d60 e conferido por FUN_103c6640. */
enum bd_hid_frame {
    BD_REPORT_LEN = 64,
    BD_REPORT_OUT = 0x81,
    BD_REPORT_IN = 0x02,
    BD_TAG_ADVANCE = 0x04,
    BD_HDR_LEN = 16,
    BD_CHUNK_MAX = 0x2D,      /* clamp de payload em FUN_103c60e0: 45 B */
};

/* 1652 bytes = 476 + 3 × 392; inteiros little-endian. */
enum bd_record_layout {
    BD_REC_LEN = 0x674,
    BD_OFF_FLAG = 0x000,      /* uint32 flag[3] — config habilitada por slot */
    BD_OFF_CRC = 0x00C,       /* uint32 crc_value: contador do device */
    BD_OFF_MODE = 0x010,      /* uint16 gamepad_mode: 0 Switch, 1 DInput, 3 XInput */
    BD_OFF_SLOT = 0x012,      /* uint16 cur_slot: o device devolve 4, não 0 */
    BD_OFF_NAME = 0x014,      /* 3 × uint8 name[32], UTF-16LE */
    BD_OFF_RUM = 0x074,       /* 3 × { uint32 flag; float zoom_out[2]; } */
    BD_OFF_JOY = 0x098,       /* 3 × { uint32 flag; uint8 range[2][2]; } */
    BD_OFF_TRG = 0x0B0,       /* idem para os gatilhos L/R */
    BD_OFF_FEAT = 0x0C8,      /* 3 × { uint32 flag; uint32 featrue; } */
    BD_OFF_MAP = 0x0E0,       /* 3 × { uint32 flag; uint32 key[20]; } */
    BD_OFF_MAC = 0x1DC,       /* 3 × 392 bytes de macro */
};

enum bd_record_slot {
    BD_NKEYS = 20,            /* 18 MAP_KEY_E + P1/P2 */
    BD_SLOTS = 3,
    BD_NAME_UNITS = 16,       /* 32 bytes / 2; a UI grava 15 + NUL */
};

/* FUN_103c5960: init 0xFFFF, polinômio refletido 0x8408 (CCITT, não ARC:
 * "123456789" → 0x916F), sem xorout, bytes trocados no retorno. */
[[nodiscard]] uint16_t bd_crc16(const uint8_t *restrict payload, size_t length);
[[nodiscard]] int bd_pid_uses_crc(uint16_t pid);

[[nodiscard]] uint16_t bd_rd16(const uint8_t *restrict record, size_t offset);
[[nodiscard]] uint32_t bd_rd32(const uint8_t *restrict record, size_t offset);
void bd_wr16(uint8_t *restrict record, size_t offset, uint16_t value);
void bd_wr32(uint8_t *restrict record, size_t offset, uint32_t value);

/* Campo de 32 B em UTF-16LE: só o BMP cabe, sem pares substitutos. */
void bd_name_get(const uint8_t *restrict record, int slot,
                 char *restrict out, size_t out_size);
void bd_name_set(uint8_t *restrict record, int slot, const char *restrict utf8);

/* Ordem do combo; valores são os bitmasks KEY_*_MAP de 04-managed.md §(c.4). */
struct bd_action {
    const char *name;
    uint32_t value;
};
extern const struct bd_action bd_actions[];
extern const size_t bd_actions_count;
extern const char *const bd_key_short[BD_NKEYS];
extern const char *const bd_key_long[BD_NKEYS];
extern const char *const bd_mode_names[4];
[[nodiscard]] int bd_action_index(uint32_t value);

/* [0x81][len+1][0x04][payload] em 64 bytes; 0 se o payload não couber. */
[[nodiscard]] size_t bd_hid_build_frame(const uint8_t *restrict payload,
                                        size_t payload_length,
                                        uint8_t out[restrict BD_REPORT_LEN]);
/* Exige report 0x02 com tag 0x04; sub/len em +4/+6, dados em +18. */
[[nodiscard]] int bd_hid_parse_resp(const uint8_t *in, size_t in_length,
                                    uint16_t *sub, uint16_t *length,
                                    const uint8_t **data);

/* 16 bytes: cmd/sub/len/crc/total/offset. Header CRC só nos PIDs de
 * bd_pid_uses_crc; nos demais passe crc = 0. */
void bd_type_c_header(uint16_t cmd, uint16_t sub, uint16_t len, uint16_t crc,
                      uint32_t total, uint32_t offset,
                      uint8_t out[restrict BD_HDR_LEN]);

#endif /* BD_CORE_H */
