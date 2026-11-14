#include "bd_core.h"

#include <string.h>
#include <strings.h>

/* FUN_103c5960 (0x103c5960, app/8BitDoAdvance.dll): init 0xFFFF, polinômio
 * refletido 0x8408 (0x1021 CCITT com os bits invertidos), sem xorout e
 * byte-swap no retorno. NÃO é CRC-16/ARC (0xA001 = 0x8005 refletido, IBM);
 * o vetor "123456789" → 0x916F (tests/test_core.c) confirma CCITT.
 *
 * _ANSI_CRC_16_Ultimate_PC (0x103c77c0) repassa (ptr, 0x674). O header Tipo C
 * recebe o crc pronto do chamador; o crc_value em 0x00C é contador do device.
 */
static constexpr uint16_t CRC16_INITIAL = 0xFFFF;
static constexpr uint16_t CRC16_REFLECTED_POLYNOMIAL = 0x8408;
static constexpr unsigned CRC16_BITS_PER_BYTE = 8;
static constexpr unsigned CRC16_BYTE_SHIFT = 8;
static constexpr uint16_t CRC16_LOW_BIT = 0x0001;

static uint16_t crc16_update_byte(uint16_t crc, uint8_t byte)
{
    crc = (uint16_t)(crc ^ (uint16_t)byte);
    for (unsigned bit = 0; bit < CRC16_BITS_PER_BYTE; ++bit) {
        const bool least_significant_bit_set = (crc & CRC16_LOW_BIT) != 0;
        crc = (uint16_t)(crc >> 1);
        if (least_significant_bit_set)
            crc = (uint16_t)(crc ^ CRC16_REFLECTED_POLYNOMIAL);
    }
    return crc;
}

uint16_t bd_crc16(const uint8_t *restrict payload, size_t length)
{
    uint16_t crc = CRC16_INITIAL;
    for (size_t index = 0; index < length; ++index)
        crc = crc16_update_byte(crc, payload[index]);
    return (uint16_t)((crc >> CRC16_BYTE_SHIFT) | (crc << CRC16_BYTE_SHIFT));
}

/* Lista literal de 0x103c60e0 (não reconferida byte a byte). */
static constexpr uint16_t CRC_PIDS[] = {
    0x6006, 0x3010, 0x3011, 0x6007, 0x3109, 0x9023, 0x9024,
};
static constexpr size_t CRC_PID_COUNT = sizeof CRC_PIDS / sizeof CRC_PIDS[0];

static bool pid_is_in_crc_table(uint16_t pid)
{
    for (size_t index = 0; index < CRC_PID_COUNT; ++index) {
        if (CRC_PIDS[index] == pid)
            return true;
    }
    return false;
}

int bd_pid_uses_crc(uint16_t pid)
{
    return pid_is_in_crc_table(pid) ? 1 : 0;
}

static constexpr size_t RECORD_FLAG_BYTES = 4;
static constexpr size_t RECORD_CRC_BYTES = 4;
static constexpr size_t RECORD_MODE_BYTES = 2;
static constexpr size_t RECORD_SLOT_BYTES = 2;
static constexpr size_t RECORD_NAME_BYTES = BD_NAME_UNITS * 2;
static constexpr size_t RECORD_RUMBLE_SLOT_BYTES = 12;
static constexpr size_t RECORD_JOY_SLOT_BYTES = 8;
static constexpr size_t RECORD_TRIGGER_SLOT_BYTES = 8;
static constexpr size_t RECORD_FEATURE_SLOT_BYTES = 8;
static constexpr size_t RECORD_MAP_KEY_SLOT_BYTES = 4 + BD_NKEYS * 4;
static constexpr size_t RECORD_MACRO_SLOT_BYTES = 392;

static_assert(BD_OFF_FLAG + BD_SLOTS * RECORD_FLAG_BYTES == BD_OFF_CRC);
static_assert(BD_OFF_CRC + RECORD_CRC_BYTES == BD_OFF_MODE);
static_assert(BD_OFF_MODE + RECORD_MODE_BYTES == BD_OFF_SLOT);
static_assert(BD_OFF_SLOT + RECORD_SLOT_BYTES == BD_OFF_NAME);
static_assert(BD_OFF_NAME + BD_SLOTS * RECORD_NAME_BYTES == BD_OFF_RUM);
static_assert(BD_OFF_RUM + BD_SLOTS * RECORD_RUMBLE_SLOT_BYTES == BD_OFF_JOY);
static_assert(BD_OFF_JOY + BD_SLOTS * RECORD_JOY_SLOT_BYTES == BD_OFF_TRG);
static_assert(BD_OFF_TRG + BD_SLOTS * RECORD_TRIGGER_SLOT_BYTES == BD_OFF_FEAT);
static_assert(BD_OFF_FEAT + BD_SLOTS * RECORD_FEATURE_SLOT_BYTES == BD_OFF_MAP);
static_assert(BD_OFF_MAP + BD_SLOTS * RECORD_MAP_KEY_SLOT_BYTES == BD_OFF_MAC);
static_assert(BD_OFF_MAC + BD_SLOTS * RECORD_MACRO_SLOT_BYTES == BD_REC_LEN);

uint16_t bd_rd16(const uint8_t *restrict record, size_t offset)
{
    return (uint16_t)((uint16_t)record[offset] |
                      (uint16_t)((uint16_t)record[offset + 1] << 8));
}

uint32_t bd_rd32(const uint8_t *restrict record, size_t offset)
{
    return (uint32_t)record[offset]
         | ((uint32_t)record[offset + 1] << 8)
         | ((uint32_t)record[offset + 2] << 16)
         | ((uint32_t)record[offset + 3] << 24);
}

void bd_wr16(uint8_t *restrict record, size_t offset, uint16_t value)
{
    record[offset] = (uint8_t)(value & 0xFFu);
    record[offset + 1] = (uint8_t)(value >> 8);
}

void bd_wr32(uint8_t *restrict record, size_t offset, uint32_t value)
{
    record[offset] = (uint8_t)(value & 0xFFu);
    record[offset + 1] = (uint8_t)((value >> 8) & 0xFFu);
    record[offset + 2] = (uint8_t)((value >> 16) & 0xFFu);
    record[offset + 3] = (uint8_t)((value >> 24) & 0xFFu);
}

enum bd_utf8_pattern {
    UTF8_ASCII_MAX = 0x7F,
    UTF8_TWO_BYTE_LEAD_MASK = 0xE0,
    UTF8_TWO_BYTE_LEAD = 0xC0,
    UTF8_TWO_BYTE_PAYLOAD_MASK = 0x1F,
    UTF8_THREE_BYTE_LEAD_MASK = 0xF0,
    UTF8_THREE_BYTE_LEAD = 0xE0,
    UTF8_THREE_BYTE_PAYLOAD_MASK = 0x0F,
    UTF8_CONTINUATION_MASK = 0x3F,
    UTF8_CONTINUATION_PREFIX = 0x80,
    UTF8_TWO_BYTE_VALUE_MAX = 0x7FF,
    UTF8_SHIFT_6_BITS = 6,
    UTF8_SHIFT_12_BITS = 12,
    UTF16_CODE_UNIT_BYTES = 2,
    UTF16_SURROGATE_FIRST = 0xD800,
    UTF16_SURROGATE_LAST = 0xDFFF,
    UTF8_MAX_SEQUENCE_BYTES = 3,
};

struct bd_utf8_code_point {
    uint32_t value;
    size_t sequence_bytes;
    bool valid;
};

/* O record guarda UTF-16LE de 16 bits (só BMP, sem pares substitutos).
 * c16/mbrtoc16 dependem da locale e não emitem UTF-16LE; o codec explícito de
 * 1..3 bytes mantém o comportamento determinístico. */
static struct bd_utf8_code_point utf8_decode_next(const char *text)
{
    const unsigned char *bytes = (const unsigned char *)text;
    const unsigned char lead = bytes[0];

    if (lead <= UTF8_ASCII_MAX)
        return (struct bd_utf8_code_point){
            .value = lead, .sequence_bytes = 1, .valid = true,
        };

    if ((lead & UTF8_TWO_BYTE_LEAD_MASK) == UTF8_TWO_BYTE_LEAD && bytes[1] != 0)
        return (struct bd_utf8_code_point){
            .value = ((uint32_t)(lead & UTF8_TWO_BYTE_PAYLOAD_MASK) << UTF8_SHIFT_6_BITS)
                   | (uint32_t)(bytes[1] & UTF8_CONTINUATION_MASK),
            .sequence_bytes = 2,
            .valid = true,
        };

    if ((lead & UTF8_THREE_BYTE_LEAD_MASK) == UTF8_THREE_BYTE_LEAD
        && bytes[1] != 0 && bytes[2] != 0)
        return (struct bd_utf8_code_point){
            .value = ((uint32_t)(lead & UTF8_THREE_BYTE_PAYLOAD_MASK) << UTF8_SHIFT_12_BITS)
                   | ((uint32_t)(bytes[1] & UTF8_CONTINUATION_MASK) << UTF8_SHIFT_6_BITS)
                   | (uint32_t)(bytes[2] & UTF8_CONTINUATION_MASK),
            .sequence_bytes = 3,
            .valid = true,
        };

    return (struct bd_utf8_code_point){ .value = 0, .sequence_bytes = 1, .valid = false };
}

static bool utf16_is_surrogate(uint32_t code_point)
{
    return code_point >= UTF16_SURROGATE_FIRST && code_point <= UTF16_SURROGATE_LAST;
}

static size_t utf8_encode_code_unit(uint16_t code_unit, char *restrict out)
{
    if (code_unit <= UTF8_ASCII_MAX) {
        out[0] = (char)code_unit;
        return 1;
    }
    if (code_unit <= UTF8_TWO_BYTE_VALUE_MAX) {
        out[0] = (char)(UTF8_TWO_BYTE_LEAD | (code_unit >> UTF8_SHIFT_6_BITS));
        out[1] = (char)(UTF8_CONTINUATION_PREFIX | (code_unit & UTF8_CONTINUATION_MASK));
        return 2;
    }
    out[0] = (char)(UTF8_THREE_BYTE_LEAD | (code_unit >> UTF8_SHIFT_12_BITS));
    out[1] = (char)(UTF8_CONTINUATION_PREFIX
                    | ((code_unit >> UTF8_SHIFT_6_BITS) & UTF8_CONTINUATION_MASK));
    out[2] = (char)(UTF8_CONTINUATION_PREFIX | (code_unit & UTF8_CONTINUATION_MASK));
    return 3;
}

static void convert_utf8_to_utf16le(const char *restrict utf8,
                                    uint8_t *restrict out, size_t max_units)
{
    size_t unit_count = 0;
    const char *cursor = utf8;

    while (*cursor != '\0' && unit_count < max_units) {
        const struct bd_utf8_code_point decoded = utf8_decode_next(cursor);
        cursor += decoded.sequence_bytes;
        if (!decoded.valid || utf16_is_surrogate(decoded.value))
            continue;
        bd_wr16(out, unit_count * UTF16_CODE_UNIT_BYTES, (uint16_t)decoded.value);
        ++unit_count;
    }
}

void bd_name_get(const uint8_t *restrict record, int slot,
                 char *restrict out, size_t out_size)
{
    if (out_size == 0)
        return;

    const size_t field_offset = BD_OFF_NAME + (size_t)slot * RECORD_NAME_BYTES;
    size_t output_length = 0;

    for (size_t unit = 0; unit < BD_NAME_UNITS; ++unit) {
        const uint16_t code_unit = bd_rd16(record, field_offset + unit * UTF16_CODE_UNIT_BYTES);
        if (code_unit == 0)
            break;
        if (utf16_is_surrogate(code_unit))
            continue;
        if (out_size - output_length <= UTF8_MAX_SEQUENCE_BYTES)
            break;
        output_length += utf8_encode_code_unit(code_unit, out + output_length);
    }
    out[output_length] = '\0';
}

void bd_name_set(uint8_t *restrict record, int slot, const char *restrict utf8)
{
    uint8_t *const field = record + BD_OFF_NAME + (size_t)slot * RECORD_NAME_BYTES;
    memset(field, 0, RECORD_NAME_BYTES);
    convert_utf8_to_utf16le(utf8 != nullptr ? utf8 : "", field, BD_NAME_UNITS - 1);
}

/* Bitmasks de KEY_*_MAP (04-managed.md §(c.4), .cctor de ConfidentialData). */
enum bd_button_mask : uint32_t {
    BD_MASK_L3 = 0x0000001,
    BD_MASK_R3 = 0x0000002,
    BD_MASK_SELECT = 0x0000004,
    BD_MASK_START = 0x0000008,
    BD_MASK_X = 0x0000010,
    BD_MASK_Y = 0x0000020,
    BD_MASK_RIGHT = 0x0000040,
    BD_MASK_LEFT = 0x0000080,
    BD_MASK_DOWN = 0x0000100,
    BD_MASK_UP = 0x0000200,
    BD_MASK_L1 = 0x0000400,
    BD_MASK_R1 = 0x0000800,
    BD_MASK_B = 0x0001000,
    BD_MASK_A = 0x0002000,
    BD_MASK_L2 = 0x0004000,
    BD_MASK_HOME = 0x0008000,
    BD_MASK_MENU = 0x0010000,
    BD_MASK_R2 = 0x0020000,
    BD_MASK_BT_CON = 0x0040000,
    BD_MASK_SCREENSHOT = 0x0400000,
    BD_MASK_TURBO_SINGLE = 0x0800000,
    BD_MASK_TURBO_AUTO = 0x1000000,
    BD_MASK_P1 = 0x2000000,
    BD_MASK_P2 = 0x4000000,
    BD_MASK_SWAP = 0x8000000,
};

/* Ordem do combo no app oficial. */
enum bd_action_id {
    BD_ACTION_NONE = 0,
    BD_ACTION_A,
    BD_ACTION_B,
    BD_ACTION_X,
    BD_ACTION_Y,
    BD_ACTION_L1,
    BD_ACTION_R1,
    BD_ACTION_L2,
    BD_ACTION_R2,
    BD_ACTION_L3,
    BD_ACTION_R3,
    BD_ACTION_SELECT,
    BD_ACTION_START,
    BD_ACTION_HOME,
    BD_ACTION_MENU,
    BD_ACTION_BT_CON,
    BD_ACTION_UP,
    BD_ACTION_DOWN,
    BD_ACTION_LEFT,
    BD_ACTION_RIGHT,
    BD_ACTION_SCREENSHOT,
    BD_ACTION_TURBO_SINGLE,
    BD_ACTION_TURBO_AUTO,
    BD_ACTION_P1,
    BD_ACTION_P2,
    BD_ACTION_SWAP,
    BD_ACTION_COUNT,
};

const struct bd_action bd_actions[] = {
    {"— (nenhum)", 0x0},
    /* valores medidos no record que o app oficial grava (map identidade) */
    {"A", 0x2000}, {"B", 0x1000}, {"X", 0x10}, {"Y", 0x20},
    {"LB", 0x400}, {"RB", 0x800}, {"LT", 0x4000}, {"RT", 0x8000},
    {"LS", 0x2}, {"RS", 0x4}, {"VIEW", 0x8}, {"MENU", 0x1},
    {"HOME", 0x20000}, {"STAR", 0x10000}, {"BT_CON", 0x40000},
    {"D_UP", 0x200}, {"D_DOWN", 0x100}, {"D_LEFT", 0x80}, {"D_RIGHT", 0x40},
    {"SCREENSHOT", 0x400000}, {"TURBO_SINGLE", 0x800000}, {"TURBO_AUTO", 0x1000000},
    {"P1", 0x2000000}, {"P2", 0x4000000}, {"SWAP", 0x8000000},
};
const size_t bd_actions_count = sizeof bd_actions / sizeof bd_actions[0];

/* O controle e layout Xbox, mas L1/L2/L3 e o vocabulario do Switch e aparece em
 * todo material da 8BitDo: os dois nomes precisam valer na linha de comando. */
static const struct {
    const char *alias;
    const char *canonical;
} bd_action_aliases[] = {
    {"L1", "LB"}, {"R1", "RB"}, {"L2", "LT"}, {"R2", "RT"},
    {"L3", "LS"}, {"R3", "RS"}, {"LEFT", "D_LEFT"}, {"RIGHT", "D_RIGHT"},
    {"UP", "D_UP"}, {"DOWN", "D_DOWN"}, {"DN", "D_DOWN"}, {"LF", "D_LEFT"},
    {"SELECT", "VIEW"}, {"START", "MENU"},
};

static_assert(sizeof bd_actions / sizeof bd_actions[0] == (size_t)BD_ACTION_COUNT);

uint32_t bd_action_value(const char *name)
{
    for (size_t i = 0; i < bd_actions_count; i++)
        if (strcasecmp(name, bd_actions[i].name) == 0) return bd_actions[i].value;

    for (size_t i = 0; i < sizeof bd_action_aliases / sizeof bd_action_aliases[0]; i++) {
        if (strcasecmp(name, bd_action_aliases[i].alias) != 0) continue;
        for (size_t a = 0; a < bd_actions_count; a++)
            if (strcmp(bd_action_aliases[i].canonical, bd_actions[a].name) == 0)
                return bd_actions[a].value;
    }
    return 0;
}

/* Ordem 0..17 de MAP_KEY_E (04-managed.md §(c.3)); P1/P2 em 18/19.
 * RT aqui é o d-pad →, não o gatilho direito. */
enum bd_key_index {
    BD_KEY_A = 0,
    BD_KEY_B,
    BD_KEY_X,
    BD_KEY_Y,
    BD_KEY_L1,
    BD_KEY_R1,
    BD_KEY_L2,
    BD_KEY_R2,
    BD_KEY_L3,
    BD_KEY_R3,
    BD_KEY_SELECT,
    BD_KEY_START,
    BD_KEY_STAR,
    BD_KEY_HOME,
    BD_KEY_UP,
    BD_KEY_DN,
    BD_KEY_LF,
    BD_KEY_RT,
    BD_KEY_P1,
    BD_KEY_P2,
};

static_assert(BD_KEY_P2 + 1 == BD_NKEYS);

const char *const bd_key_short[BD_NKEYS] = {
    [BD_KEY_A] = "A",
    [BD_KEY_B] = "B",
    [BD_KEY_X] = "X",
    [BD_KEY_Y] = "Y",
    [BD_KEY_L1] = "LB",
    [BD_KEY_R1] = "RB",
    [BD_KEY_L2] = "LT",
    [BD_KEY_R2] = "RT",
    [BD_KEY_L3] = "LS",
    [BD_KEY_R3] = "RS",
    [BD_KEY_SELECT] = "VIEW",
    [BD_KEY_START] = "MENU",
    [BD_KEY_STAR] = "STAR",
    [BD_KEY_HOME] = "HOME",
    [BD_KEY_UP] = "D_UP",
    [BD_KEY_DN] = "D_DOWN",
    [BD_KEY_LF] = "D_LEFT",
    [BD_KEY_RT] = "D_RIGHT",
    [BD_KEY_P1] = "P1",
    [BD_KEY_P2] = "P2",
};

const char *const bd_key_long[BD_NKEYS] = {
    [BD_KEY_A] = "A",
    [BD_KEY_B] = "B",
    [BD_KEY_X] = "X",
    [BD_KEY_Y] = "Y",
    [BD_KEY_L1] = "LB (bumper esq.)",
    [BD_KEY_R1] = "RB (bumper dir.)",
    [BD_KEY_L2] = "LT (gatilho esq.)",
    [BD_KEY_R2] = "RT (gatilho dir.)",
    [BD_KEY_L3] = "LS (clique analógico esq.)",
    [BD_KEY_R3] = "RS (clique analógico dir.)",
    [BD_KEY_SELECT] = "VIEW (−)",
    [BD_KEY_START] = "MENU (+)",
    [BD_KEY_STAR] = "STAR (★)",
    [BD_KEY_HOME] = "HOME",
    [BD_KEY_UP] = "d-pad ↑",
    [BD_KEY_DN] = "d-pad ↓",
    [BD_KEY_LF] = "d-pad ←",
    [BD_KEY_RT] = "d-pad →",
    [BD_KEY_P1] = "P1 (traseiro esq.)",
    [BD_KEY_P2] = "P2 (traseiro dir.)",
};

/* gamepad_mode: 0 Switch, 1 DInput, 3 XInput; 2 não observado (03-profile.md). */
enum bd_mode_id {
    BD_MODE_SWITCH = 0,
    BD_MODE_DINPUT,
    BD_MODE_UNKNOWN,
    BD_MODE_XINPUT,
    BD_MODE_COUNT,
};

const char *const bd_mode_names[BD_MODE_COUNT] = {
    [BD_MODE_SWITCH] = "Switch",
    [BD_MODE_DINPUT] = "DInput",
    [BD_MODE_UNKNOWN] = "(?)",
    [BD_MODE_XINPUT] = "XInput",
};

static_assert(BD_MODE_COUNT == 4);

int bd_action_index(uint32_t value)
{
    for (size_t index = 0; index < bd_actions_count; ++index) {
        if (bd_actions[index].value == value)
            return (int)index;
    }
    return BD_ACTION_NONE;
}

static constexpr size_t HID_FRAME_OFF_REPORT_ID = 0;
static constexpr size_t HID_FRAME_OFF_LENGTH = 1;
static constexpr size_t HID_FRAME_OFF_TAG = 2;
static constexpr size_t HID_FRAME_OFF_PAYLOAD = 3;
static constexpr size_t HID_FRAME_HEADER_BYTES = 3;
static constexpr size_t HID_FRAME_LENGTH_BIAS = 1;

static constexpr size_t HID_RESPONSE_OFF_REPORT_ID = 0;
static constexpr size_t HID_RESPONSE_OFF_TAG = 1;
static constexpr size_t HID_RESPONSE_OFF_SUBCOMMAND = 4;
static constexpr size_t HID_RESPONSE_OFF_LENGTH = 6;
static constexpr size_t HID_RESPONSE_OFF_PAYLOAD = 18;

static constexpr size_t TYPE_C_OFF_CMD = 0;
static constexpr size_t TYPE_C_OFF_SUBCOMMAND = 2;
static constexpr size_t TYPE_C_OFF_LENGTH = 4;
static constexpr size_t TYPE_C_OFF_CRC = 6;
static constexpr size_t TYPE_C_OFF_TOTAL = 8;
static constexpr size_t TYPE_C_OFF_OFFSET = 12;

static_assert(BD_HDR_LEN + BD_CHUNK_MAX + HID_FRAME_HEADER_BYTES <= BD_REPORT_LEN);

uint16_t bd_chunk_len(uint32_t offset)
{
    if (offset >= BD_REC_LEN)
        return 0;
    const uint32_t remaining = BD_REC_LEN - offset;
    return (uint16_t)(remaining < BD_CHUNK_MAX ? remaining : BD_CHUNK_MAX);
}

size_t bd_hid_build_frame(const uint8_t *restrict payload, size_t payload_length,
                          uint8_t out[restrict BD_REPORT_LEN])
{
    if (payload_length > BD_REPORT_LEN - HID_FRAME_HEADER_BYTES)
        return 0;

    memset(out, 0, BD_REPORT_LEN);
    out[HID_FRAME_OFF_REPORT_ID] = BD_REPORT_OUT;
    out[HID_FRAME_OFF_LENGTH] = (uint8_t)(payload_length + HID_FRAME_LENGTH_BIAS);
    out[HID_FRAME_OFF_TAG] = BD_TAG_ADVANCE;
    if (payload_length > 0)
        memcpy(out + HID_FRAME_OFF_PAYLOAD, payload, payload_length);
    return payload_length + HID_FRAME_HEADER_BYTES;
}

int bd_hid_parse_resp(const uint8_t *in, size_t in_length,
                      uint16_t *sub, uint16_t *length, const uint8_t **data)
{
    if (in_length < BD_REPORT_LEN)
        return 0;
    if (in[HID_RESPONSE_OFF_REPORT_ID] != BD_REPORT_IN)
        return 0;
    if (in[HID_RESPONSE_OFF_TAG] != BD_TAG_ADVANCE)
        return 0;

    if (sub != nullptr)
        *sub = bd_rd16(in, HID_RESPONSE_OFF_SUBCOMMAND);
    if (length != nullptr)
        *length = bd_rd16(in, HID_RESPONSE_OFF_LENGTH);
    if (data != nullptr)
        *data = in + HID_RESPONSE_OFF_PAYLOAD;
    return 1;
}

void bd_type_c_header(uint16_t cmd, uint16_t sub, uint16_t len, uint16_t crc,
                      uint32_t total, uint32_t offset,
                      uint8_t out[restrict BD_HDR_LEN])
{
    bd_wr16(out, TYPE_C_OFF_CMD, cmd);
    bd_wr16(out, TYPE_C_OFF_SUBCOMMAND, sub);
    bd_wr16(out, TYPE_C_OFF_LENGTH, len);
    bd_wr16(out, TYPE_C_OFF_CRC, crc);
    bd_wr32(out, TYPE_C_OFF_TOTAL, total);
    bd_wr32(out, TYPE_C_OFF_OFFSET, offset);
}

