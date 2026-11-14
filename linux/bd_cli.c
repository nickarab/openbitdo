#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bd_hid.h"

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
            "     %s info\n",
            program, program, program, program);
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
    if (strcmp(command, "write") == 0)
        return command_write(argc - 2, argv + 2);
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
