#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "bd_hid.h"
#include "bd_usb.h"

static constexpr uint16_t READ_COMMAND = 0x0002;
static constexpr uint16_t WRITE_COMMAND = 0x0001;
static constexpr uint32_t STATE_LENGTH = 0x14; /* bloco lido por getUltimate_*CurrentSlot */
static constexpr size_t DEVICE_CRC_BYTES = 4;
static constexpr size_t DEVICE_SLOT_BYTES = 2;

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "uso: %s list\n"
            "     %s dump <arquivo>\n"
            "     %s write <arquivo> [--verify] [--dry-run] [--no-commit]\n"
            "            [--first-chunk] [--pid N] [--device CAMINHO]\n"
            "     %s patch <slot> [--p1 NOME] [--p2 NOME] [--mode 0|1|3]\n"
            "     %s hid                       ativa o HID (XInput -> DInput)\n"
            "     %s mode <1|2|3|4>            changeDinput_CMD (1 DInput, 2 XInput)\n"
            "     %s info\n",
            program, program, program, program, program, program, program);
}

[[nodiscard]] static bool parse_pid(const char *text, uint16_t *pid)
{
    if (text[0] == '-')
        return false;
    char *end = nullptr;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value > UINT16_MAX)
        return false;
    *pid = (uint16_t)value;
    return true;
}

[[nodiscard]] static bool copy_path(char destination[BD_HID_PATH_SIZE], const char *path)
{
    int length = snprintf(destination, BD_HID_PATH_SIZE, "%s", path);
    if (length < 0 || length >= BD_HID_PATH_SIZE) {
        fprintf(stderr, "caminho longo: %s\n", path);
        return false;
    }
    return true;
}

[[nodiscard]] static bool select_device(struct bd_hid_device *selected,
                                        const char *requested_path,
                                        bool pid_given, uint16_t pid)
{
    struct bd_hid_device devices[BD_HID_MAX_DEVICES];
    int count = bd_hid_find(devices, BD_HID_MAX_DEVICES);
    if (count < 0) {
        fprintf(stderr, "falha ao enumerar /sys/class/hidraw\n");
        return false;
    }

    if (requested_path != nullptr) {
        if (!copy_path(selected->path, requested_path))
            return false;
        selected->pid = pid_given ? pid : 0;
        for (int index = 0; index < count; ++index) {
            if (strcmp(devices[index].path, requested_path) == 0) {
                if (!pid_given)
                    selected->pid = devices[index].pid;
                return true;
            }
        }
        if (pid_given)
            return true;
        fprintf(stderr, "%s: PID desconhecido; use --pid\n", requested_path);
        return false;
    }

    if (count == 0) {
        fprintf(stderr, "nenhum 8BitDo (VID %04x) em /sys/class/hidraw\n", (unsigned)BD_VID);
        return false;
    }
    if (!copy_path(selected->path, devices[0].path))
        return false;
    selected->pid = pid_given ? pid : devices[0].pid;
    return true;
}

[[nodiscard]] static bool open_device(struct bd_hid_link *link,
                                      const struct bd_hid_device *device)
{
    if (!bd_hid_open(link, device->path, device->pid)) {
        fprintf(stderr, "não abri %s\n", device->path);
        return false;
    }
    /* o app oficial sempre abre a sessão antes de ler/gravar (ver docs/protocol.md) */
    if (!bd_hid_open_session(link))
        fprintf(stderr, "aviso: sessão de configuração não confirmou ACK\n");
    return true;
}

[[nodiscard]] static bool load_record(const char *path, uint8_t record[BD_REC_LEN])
{
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return false;
    }
    size_t read_size = fread(record, 1, (size_t)BD_REC_LEN, file);
    int extra = fgetc(file);
    fclose(file);
    if (read_size != (size_t)BD_REC_LEN) {
        fprintf(stderr, "%s: %zu de %u bytes\n", path, read_size, (unsigned)BD_REC_LEN);
        return false;
    }
    if (extra != EOF) {
        fprintf(stderr, "%s: maior que %u bytes\n", path, (unsigned)BD_REC_LEN);
        return false;
    }
    return true;
}

static int command_list(void)
{
    struct bd_hid_device devices[BD_HID_MAX_DEVICES];
    int count = bd_hid_find(devices, BD_HID_MAX_DEVICES);
    if (count < 0) {
        fprintf(stderr, "falha ao enumerar /sys/class/hidraw\n");
        return 1;
    }
    if (count == 0) {
        fprintf(stderr, "nenhum 8BitDo (VID %04x) em /sys/class/hidraw\n", (unsigned)BD_VID);
        return 0;
    }
    for (int index = 0; index < count; ++index)
        printf("%s  %04x:%04x\n", devices[index].path,
               (unsigned)BD_VID, (unsigned)devices[index].pid);
    return 0;
}

static int command_dump(const char *path)
{
    struct bd_hid_device selected;
    if (!select_device(&selected, nullptr, false, 0))
        return 1;

    struct bd_hid_link link = {.fd = -1, .pid = selected.pid};
    if (!open_device(&link, &selected))
        return 1;
    uint8_t record[BD_REC_LEN];
    bool read = bd_hid_read_record(&link, record, nullptr);
    (void)bd_hid_close_session(&link);
    bd_hid_close(&link);
    if (!read) {
        fprintf(stderr, "leitura do record falhou\n");
        return 1;
    }

    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return 1;
    }
    bool written = fwrite(record, 1, (size_t)BD_REC_LEN, file) == (size_t)BD_REC_LEN;
    if (fclose(file) != 0)
        written = false;
    if (!written) {
        fprintf(stderr, "%s: escrita falhou\n", path);
        return 1;
    }
    printf("%s: %u bytes\n", path, (unsigned)BD_REC_LEN);
    return 0;
}

static int command_info(void)
{
    struct bd_hid_device selected;
    if (!select_device(&selected, nullptr, false, 0))
        return 1;

    struct bd_hid_link link = {.fd = -1, .pid = selected.pid};
    if (!open_device(&link, &selected))
        return 1;
    uint8_t state[STATE_LENGTH];
    int state_length = 0;
    bool read = bd_hid_execute(&link, READ_COMMAND, 0, STATE_LENGTH, 0, nullptr,
                               (uint16_t)STATE_LENGTH, state, (int)sizeof state,
                               &state_length, nullptr);
    (void)bd_hid_close_session(&link);
    bd_hid_close(&link);
    if (!read) {
        fprintf(stderr, "sem resposta ao cmd 0x%04x\n", (unsigned)READ_COMMAND);
        return 1;
    }

    printf("%s  VID:PID %04x:%04x  header-crc: %s\n", selected.path,
           (unsigned)BD_VID, (unsigned)selected.pid,
           bd_pid_uses_crc(selected.pid) ? "sim" : "não");
    if (state_length > 0) {
        printf("estado (%d bytes):", state_length);
        for (int index = 0; index < state_length; ++index)
            printf(" %02x", (unsigned)state[index]);
        printf("\nslot: %u\n", (unsigned)state[0]);
    } else {
        printf("estado: vazio\n");
    }
    return 0;
}

/* bd_hid_write_record sempre envia o commit (cmd 6/sub 0x15); --no-commit e
 * --first-chunk reutilizam o chunk de escrita (cmd 1) via bd_hid_execute. */
[[nodiscard]] static bool send_chunks(struct bd_hid_link *link, const uint8_t *record,
                                      bool first_chunk_only)
{
    uint32_t offset = 0;
    while (offset < (uint32_t)BD_REC_LEN) {
        size_t remaining = (size_t)BD_REC_LEN - (size_t)offset;
        uint16_t chunk = remaining > (size_t)BD_CHUNK_MAX
                             ? (uint16_t)BD_CHUNK_MAX
                             : (uint16_t)remaining;
        if (!bd_hid_execute(link, WRITE_COMMAND, 0, (uint32_t)BD_REC_LEN, offset,
                            record + (size_t)offset, chunk, nullptr, 0, nullptr, nullptr)) {
            fprintf(stderr, "chunk @%u: sem ACK\n", (unsigned)offset);
            return false;
        }
        offset += (uint32_t)chunk;
        if (first_chunk_only)
            break;
    }
    return true;
}

[[nodiscard]] static bool is_device_managed(size_t offset)
{
    return (offset >= (size_t)BD_OFF_CRC && offset < (size_t)BD_OFF_CRC + DEVICE_CRC_BYTES) ||
           (offset >= (size_t)BD_OFF_SLOT && offset < (size_t)BD_OFF_SLOT + DEVICE_SLOT_BYTES);
}

/* crc_value (0x00C) é contador do device e cur_slot (0x012) fica em 4:
 * comparar esses dois campos com o arquivo acusaria diferença sempre. */
[[nodiscard]] static bool verify_record(struct bd_hid_link *link, const uint8_t *expected)
{
    uint8_t actual[BD_REC_LEN];
    if (!bd_hid_read_record(link, actual, nullptr)) {
        fprintf(stderr, "verify: releitura falhou\n");
        return false;
    }

    size_t differences = 0;
    size_t first = 0;
    for (size_t index = 0; index < (size_t)BD_REC_LEN; ++index) {
        if (is_device_managed(index) || expected[index] == actual[index])
            continue;
        if (differences == 0)
            first = index;
        ++differences;
    }
    if (differences > 0) {
        fprintf(stderr, "verify: %zu byte(s) fora dos campos do device; primeiro em 0x%03zx\n",
                differences, first);
        return false;
    }
    printf("verify: ok | crc_value no device 0x%08x | cur_slot %u\n",
           (unsigned)bd_rd32(actual, BD_OFF_CRC),
           (unsigned)bd_rd16(actual, BD_OFF_SLOT));
    return true;
}



/* Sem hidraw (controle em XInput) troca para DInput pela interface vendor, para
 * patch/write funcionarem sem o usuario apertar HOME. */
static bool ensure_hid(struct bd_hid_device *devices)
{
    if (bd_hid_find(devices, BD_HID_MAX_DEVICES) > 0) return true;
    if (!bd_usb_has_xinput()) return false;
    printf("controle em XInput: ativando o HID (DInput)...\n");
    if (!bd_usb_switch_mode(1)) return false;
    for (int attempt = 0; attempt < 60; attempt++) {
        usleep(100000);
        if (bd_hid_find(devices, BD_HID_MAX_DEVICES) > 0) return true;
    }
    return false;
}

/* Lê o record, altera SÓ o slot indicado (P1/P2 e nome) e grava — tudo numa
 * sessão, como o app oficial faz. Preserva os outros perfis. */
static int command_patch(int argc, char **argv)
{
    if (argc < 1) return 2;
    const int slot = atoi(argv[0]);
    if (slot < 0 || slot >= BD_SLOTS) {
        fprintf(stderr, "slot inválido: %s (0..%d)\n", argv[0], BD_SLOTS - 1);
        return 2;
    }
    const char *p1_name = nullptr;
    const char *p2_name = nullptr;
    const char *mode_name = nullptr;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--p1") == 0) p1_name = argv[i + 1];
        else if (strcmp(argv[i], "--p2") == 0) p2_name = argv[i + 1];
        else if (strcmp(argv[i], "--mode") == 0) mode_name = argv[i + 1];
        else { fprintf(stderr, "opção desconhecida: %s\n", argv[i]); return 2; }
    }

    struct bd_hid_device devices[BD_HID_MAX_DEVICES];
    if (!ensure_hid(devices)) {
        fprintf(stderr, "nenhum controle 2dc8 com HID encontrado.\n"
                        "Em XInput (2dc8:3106) o controle nao expoe HID: aperte HOME ou\n"
                        "replugue o cabo. Para o HID ficar sempre disponivel, grave com\n"
                        "  bd_cli patch <slot> --mode 1   (DInput)\n");
        return 1;
    }
    struct bd_hid_link link = {.fd = -1, .pid = devices[0].pid};
    if (!bd_hid_open(&link, devices[0].path, devices[0].pid)) {
        fprintf(stderr, "não abri %s\n", devices[0].path);
        return 1;
    }
    if (!bd_hid_open_session(&link)) {
        fprintf(stderr, "sessão de configuração não abriu\n");
        bd_hid_close(&link);
        return 1;
    }

    uint8_t record[BD_REC_LEN];
    if (!bd_hid_read_record(&link, record, nullptr)) {
        fprintf(stderr, "leitura do record falhou\n");
        (void)bd_hid_close_session(&link);
        bd_hid_close(&link);
        return 1;
    }

    const size_t map_at = BD_OFF_MAP + (size_t)slot * 84 + 4;
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    if (p1_name != nullptr) {
        for (size_t a = 0; a < bd_actions_count; a++)
            if (strcasecmp(bd_actions[a].name, p1_name) == 0) p1 = bd_actions[a].value;
        bd_wr32(record, map_at + 18 * 4, p1);
    }
    if (p2_name != nullptr) {
        for (size_t a = 0; a < bd_actions_count; a++)
            if (strcasecmp(bd_actions[a].name, p2_name) == 0) p2 = bd_actions[a].value;
        bd_wr32(record, map_at + 19 * 4, p2);
    }

    bd_wr32(record, BD_OFF_FLAG + (size_t)slot * 4, BD_FEATURE_MASK);
    bd_wr32(record, BD_OFF_MAP + (size_t)slot * 84, BD_FEATURE_MASK);
    char name[BD_NAME_UNITS];
    snprintf(name, sizeof name, "Profile %d", slot + 1);
    bd_name_set(record, slot, name);
    bd_wr16(record, BD_OFF_SLOT, (uint16_t)slot);
    if (mode_name != nullptr) {
        uint32_t mode = mode_name[0] == '1' ? 1u : (mode_name[0] == '0' ? 0u : 3u);
        bd_wr32(record, BD_OFF_MODE, mode);
    }

    const bool written = bd_hid_write_record(&link, record);
    printf("escrita: %s\n", written ? "chunks + commit ACK" : "FALHOU");

    uint8_t back[BD_REC_LEN];
    if (written && bd_hid_read_record(&link, back, nullptr)) {
        size_t diff = 0;
        for (size_t i = 0; i < BD_REC_LEN; i++) {
            if (i >= BD_OFF_CRC && i < BD_OFF_CRC + 4) continue;
            if (back[i] != record[i]) diff++;
        }
        printf("releitura: %zu byte(s) divergente(s) fora do contador\n", diff);
    }

    (void)bd_hid_close_session(&link);
    bd_hid_close(&link);
    return written ? 0 : 1;
}

static int command_write(int argc, char **argv)
{
    if (argc == 0) {
        fprintf(stderr, "falta o arquivo\n");
        return 2;
    }
    const char *record_path = argv[0];
    bool verify = false;
    bool dry_run = false;
    bool no_commit = false;
    bool first_chunk = false;
    bool pid_given = false;
    uint16_t pid = 0;
    const char *device_path = nullptr;

    for (int index = 1; index < argc; ++index) {
        const char *option = argv[index];
        if (strcmp(option, "--verify") == 0) {
            verify = true;
        } else if (strcmp(option, "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(option, "--no-commit") == 0) {
            no_commit = true;
        } else if (strcmp(option, "--first-chunk") == 0) {
            first_chunk = true;
        } else if (strcmp(option, "--pid") == 0) {
            if (index + 1 >= argc || !parse_pid(argv[index + 1], &pid)) {
                fprintf(stderr, "--pid inválido\n");
                return 2;
            }
            pid_given = true;
            ++index;
        } else if (strcmp(option, "--device") == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "--device sem caminho\n");
                return 2;
            }
            device_path = argv[++index];
        } else {
            fprintf(stderr, "opção desconhecida: %s\n", option);
            return 2;
        }
    }

    uint8_t record[BD_REC_LEN];
    if (!load_record(record_path, record))
        return 1;

    printf("%s: %u bytes | modo=%u | cur_slot=%u | crc_value=0x%08x\n", record_path,
           (unsigned)BD_REC_LEN, (unsigned)bd_rd16(record, BD_OFF_MODE),
           (unsigned)bd_rd16(record, BD_OFF_SLOT),
           (unsigned)bd_rd32(record, BD_OFF_CRC));
    if (dry_run) {
        printf("dry-run: nenhum comando enviado\n");
        return 0;
    }

    struct bd_hid_device selected;
    if (!select_device(&selected, device_path, pid_given, pid))
        return 1;
    struct bd_hid_link link = {.fd = -1, .pid = selected.pid};
    if (!open_device(&link, &selected))
        return 1;

    bool written;
    if (first_chunk || no_commit) {
        written = send_chunks(&link, record, first_chunk);
        if (written) {
            if (first_chunk)
                printf("primeiro chunk confirmado; commit omitido\n");
            else
                printf("chunks confirmados; commit omitido (--no-commit)\n");
        }
    } else {
        written = bd_hid_write_record(&link, record);
        if (written)
            printf("chunks e commit confirmados\n");
    }

    bool verified = true;
    if (written && verify)
        verified = verify_record(&link, record);
    (void)bd_hid_close_session(&link);
    bd_hid_close(&link);

    if (!written) {
        fprintf(stderr, "gravação falhou\n");
        return 1;
    }
    return verified ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(stderr, argv[0]);
        return 2;
    }
    const char *command = argv[1];

    if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        print_usage(stdout, argv[0]);
        return 0;
    }
    if (strcmp(command, "list") == 0) {
        if (argc != 2) {
            fprintf(stderr, "list não aceita argumentos\n");
            return 2;
        }
        return command_list();
    }
    if (strcmp(command, "dump") == 0) {
        if (argc != 3) {
            fprintf(stderr, "uso: %s dump <arquivo>\n", argv[0]);
            return 2;
        }
        return command_dump(argv[2]);
    }
    if (strcmp(command, "patch") == 0)
        return command_patch(argc - 2, argv + 2);
    if (strcmp(command, "write") == 0)
        return command_write(argc - 2, argv + 2);
    if (strcmp(command, "send-a") == 0) {
        if (argc != 4) {
            fprintf(stderr, "uso: %s send-a <comando> <parametro>\n", argv[0]);
            return 2;
        }
        struct bd_hid_device devices[BD_HID_MAX_DEVICES];
        if (bd_hid_find(devices, BD_HID_MAX_DEVICES) <= 0) {
            fprintf(stderr, "nenhum controle 2dc8 encontrado\n");
            return 1;
        }
        struct bd_hid_link link = {.fd = -1, .pid = devices[0].pid};
        if (!bd_hid_open(&link, devices[0].path, devices[0].pid)) {
            fprintf(stderr, "não abri %s\n", devices[0].path);
            return 1;
        }
        (void)bd_hid_open_session(&link);
        uint8_t cmd = (uint8_t)strtoul(argv[2], nullptr, 0);
        uint32_t param = (uint32_t)strtoul(argv[3], nullptr, 0);
        bool acked = bd_hid_type_a(&link, cmd, param);
        printf("send-a cmd=0x%02x param=%u -> %s\n", cmd, param,
               acked ? "ACK" : "sem ACK");
        (void)bd_hid_close_session(&link);
    bd_hid_close(&link);
        return acked ? 0 : 1;
    }
    if (strcmp(command, "hid") == 0) {
        if (argc != 2) {
            fprintf(stderr, "hid nao aceita argumentos\n");
            return 2;
        }
        struct bd_hid_device devices[BD_HID_MAX_DEVICES];
        if (bd_hid_find(devices, BD_HID_MAX_DEVICES) > 0) {
            printf("HID ja disponivel: %s\n", devices[0].path);
            return 0;
        }
        if (!bd_usb_has_xinput()) {
            fprintf(stderr, "sem 2dc8:3106 e sem hidraw: conecte o controle por cabo\n");
            return 1;
        }
        printf("controle em XInput: mandando changeDinput_CMD (DInput)...\n");
        if (!bd_usb_switch_mode(1)) return 1;
        for (int attempt = 0; attempt < 60; attempt++) {
            usleep(100000);
            if (bd_hid_find(devices, BD_HID_MAX_DEVICES) > 0) {
                printf("HID disponivel em %s\n", devices[0].path);
                return 0;
            }
        }
        fprintf(stderr, "o controle nao re-enumerou como 3012\n");
        return 1;
    }
    if (strcmp(command, "mode") == 0) {
        if (argc != 3) {
            fprintf(stderr, "uso: %s mode <1=DInput 2=XInput 3=DS4 4=Switch>\n", argv[0]);
            return 2;
        }
        const unsigned long mode = strtoul(argv[2], nullptr, 10);
        if (mode < 1 || mode > 4) {
            fprintf(stderr, "modo invalido: %s\n", argv[2]);
            return 2;
        }
        struct bd_hid_device devices[BD_HID_MAX_DEVICES];
        if (bd_hid_find(devices, BD_HID_MAX_DEVICES) <= 0) {
            if (!bd_usb_switch_mode((uint8_t)mode)) {
                fprintf(stderr, "sem HID e sem 2dc8:3106 para trocar de modo\n");
                return 1;
            }
            printf("changeDinput_CMD mode=%lu -> enviado no endpoint vendor\n", mode);
            return 0;
        }
        struct bd_hid_link link = {.fd = -1, .pid = devices[0].pid};
        if (!bd_hid_open(&link, devices[0].path, devices[0].pid)) return 1;
        const bool sent = bd_hid_mode_switch(&link, (uint8_t)mode);
        printf("changeDinput_CMD mode=%lu -> %s\n", mode, sent ? "enviado" : "falhou");
        bd_hid_close(&link);
        return sent ? 0 : 1;
    }
    if (strcmp(command, "info") == 0) {
        if (argc != 2) {
            fprintf(stderr, "info não aceita argumentos\n");
            return 2;
        }
        return command_info();
    }

    fprintf(stderr, "subcomando desconhecido: %s\n", command);
    print_usage(stderr, argv[0]);
    return 2;
}
