/* Implementação de bd_hid.h. Os limites de retentativa abaixo são os medidos
 * no hardware (../re/14-write-persist.md §"A causa real do travamento"): com o
 * device parado, write() não drena e só POLLOUT libera a próxima tentativa. */
#define _GNU_SOURCE
#include "bd_hid.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

constexpr unsigned HID_BUS_USB = 0x0003;
constexpr uint16_t HID_COMMAND_READ = 0x0002;
constexpr uint16_t HID_COMMAND_WRITE = 0x0001;
constexpr uint16_t HID_COMMAND_COMMIT = 0x0006;
constexpr uint16_t HID_COMMIT_SUBCOMMAND = 0x0015;
constexpr int HID_RESPONSE_DATA_OFFSET = 18;
constexpr int HID_SEND_ATTEMPTS = 20;
constexpr int HID_SEND_POLL_MS = 100;
constexpr int HID_REPLY_ATTEMPTS = 4;
constexpr int HID_REPLY_TIMEOUT_MS = 150;
constexpr int READ_FAST_FAIL_ATTEMPTS = 2;
constexpr int READ_MAX_CONSECUTIVE_FAILURES = 8;
constexpr int WRITE_MAX_CONSECUTIVE_FAILURES = 30;
constexpr unsigned WRITE_RETRY_DELAY_US = 20000;

int bd_hid_find(struct bd_hid_device *devices, int capacity)
{
    DIR *directory = opendir("/sys/class/hidraw");
    if (!directory) return -1;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr && count < capacity) {
        if (strncmp(entry->d_name, "hidraw", sizeof "hidraw" - 1) != 0) continue;

        char uevent_path[512];
        snprintf(uevent_path, sizeof uevent_path,
                 "/sys/class/hidraw/%.60s/device/uevent", entry->d_name);
        FILE *uevent = fopen(uevent_path, "r");
        if (!uevent) continue;

        char line[256];
        unsigned bus = 0, vid = 0, pid = 0;
        while (fgets(line, sizeof line, uevent) != nullptr)
            if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3) break;
        fclose(uevent);

        if (bus != HID_BUS_USB || vid != BD_VID) continue;
        snprintf(devices[count].path, sizeof devices[count].path, "/dev/%.48s",
                 entry->d_name);
        devices[count].pid = (uint16_t)pid;
        count++;
    }
    closedir(directory);
    return count;
}

bool bd_hid_open(struct bd_hid_link *link, const char *path, uint16_t pid)
{
    link->fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    link->pid = pid;
    return link->fd >= 0;
}

void bd_hid_close(struct bd_hid_link *link)
{
    if (link->fd >= 0) close(link->fd);
    link->fd = -1;
}

[[nodiscard]] static bool link_send(const struct bd_hid_link *link, const uint8_t *payload,
                                    size_t length)
{
    uint8_t frame[BD_REPORT_LEN];
    if (bd_hid_build_frame(payload, length, frame) == 0) return false;
    for (int attempt = 0; attempt < HID_SEND_ATTEMPTS; attempt++) {
        ssize_t written = write(link->fd, frame, sizeof frame);
        if (written == (ssize_t)sizeof frame) return true;
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        struct pollfd writable = {.fd = link->fd, .events = POLLOUT};
        if (poll(&writable, 1, HID_SEND_POLL_MS) <= 0) return false;
    }
    return false;
}

[[nodiscard]] static bool link_recv(const struct bd_hid_link *link, uint8_t *buffer,
                                    int timeout_ms)
{
    struct pollfd readable = {.fd = link->fd, .events = POLLIN};
    if (poll(&readable, 1, timeout_ms) <= 0) return false;
    ssize_t received = read(link->fd, buffer, BD_REPORT_LEN);
    return received >= 0 && bd_hid_parse_resp(buffer, (size_t)received, nullptr, nullptr,
                                              nullptr) != 0;
}

/* Tipo C: header de 16 bytes + payload; o device responde ecoando o cmd. */
bool bd_hid_execute(struct bd_hid_link *link, uint16_t command, uint16_t subcommand,
                    uint32_t total, uint32_t offset, const uint8_t *data,
                    uint16_t length, uint8_t *out, int out_capacity,
                    int *out_length, volatile int *progress)
{
    uint8_t payload[BD_HDR_LEN + BD_CHUNK_MAX];
    uint16_t crc = bd_pid_uses_crc(link->pid) ? bd_crc16(data, data ? length : 0) : 0;
    bd_type_c_header(command, subcommand, length, crc, total, offset, payload);
    if (data && length > 0) memcpy(payload + BD_HDR_LEN, data, length);
    if (!link_send(link, payload, BD_HDR_LEN + (data ? length : 0))) return false;

    for (int attempt = 0; attempt < HID_REPLY_ATTEMPTS; attempt++) {
        uint8_t response[BD_REPORT_LEN];
        if (!link_recv(link, response, HID_REPLY_TIMEOUT_MS)) continue;

        uint16_t response_command = 0;
        uint16_t response_length = 0;
        const uint8_t *response_data = nullptr;
        if (bd_hid_parse_resp(response, sizeof response, &response_command,
                              &response_length, &response_data) == 0)
            continue;
        if (response_command != command) continue;

        if (progress) *progress = (int)offset;
        if (!out || out_capacity <= 0) return true;

        int available = BD_REPORT_LEN - HID_RESPONSE_DATA_OFFSET;
        int copy = response_length < available ? response_length : available;
        if (copy > out_capacity) copy = out_capacity;
        memcpy(out, response_data, (size_t)copy);
        if (out_length) *out_length = copy;
        return true;
    }
    return false;
}

bool bd_hid_read_record(struct bd_hid_link *link, uint8_t *record, volatile int *progress)
{
    uint32_t offset = 0;
    int consecutive_failures = 0;
    while (offset < BD_REC_LEN) {
        uint32_t remaining = BD_REC_LEN - offset;
        uint16_t chunk = remaining > BD_CHUNK_MAX ? BD_CHUNK_MAX : (uint16_t)remaining;
        uint8_t chunk_data[BD_REPORT_LEN];
        int received = 0;
        const bool acknowledged = bd_hid_execute(link, HID_COMMAND_READ, 0, BD_REC_LEN, offset,
                                                 nullptr, chunk, chunk_data,
                                                 (int)sizeof chunk_data, &received,
                                                 nullptr);
        if (!acknowledged || received <= 0) {
            consecutive_failures++;
            /* device mudo no primeiro chunk: falha rápido em vez de insistir por 30 s */
            if (offset == 0 && consecutive_failures >= READ_FAST_FAIL_ATTEMPTS) return false;
            if (consecutive_failures > READ_MAX_CONSECUTIVE_FAILURES) return false;
            continue;
        }
        memcpy(record + offset, chunk_data, (size_t)received);
        offset += (uint32_t)received;
        if (progress) *progress = (int)offset;
        consecutive_failures = 0;
    }
    return true;
}

/* escreve um report de 64 B exatamente como recebido (frames de controle têm
 * tag 0x00 e não seguem o envelope 04 — ver docs/protocol.md). */
static bool link_send_raw(const struct bd_hid_link *link, const uint8_t frame[BD_REPORT_LEN])
{
    for (int attempt = 0; attempt < HID_SEND_ATTEMPTS; attempt++) {
        ssize_t written = write(link->fd, frame, BD_REPORT_LEN);
        if (written == (ssize_t)BD_REPORT_LEN) return true;
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        struct pollfd writable = {.fd = link->fd, .events = POLLOUT};
        if (poll(&writable, 1, HID_SEND_POLL_MS) <= 0) return false;
    }
    return false;
}

/* changeDinput_CMD: a troca de modo do firmware. m = 1 DInput, 2 XInput, 3 DS4,
 * 4 Switch (tabela do app oficial, classe DinputHid). Vai como frame cru. */
bool bd_hid_mode_switch(struct bd_hid_link *link, uint8_t mode)
{
    uint8_t frame[BD_REPORT_LEN] = {0x81, 0x05, 0x00, 0x51, mode};
    return link_send_raw(link, frame);
}

bool bd_hid_open_session(struct bd_hid_link *link)
{
    uint8_t enter[BD_REPORT_LEN] = {0x81, 0x05, 0x00, 0x21, 0x01};
    if (!link_send_raw(link, enter)) return false;
    /* o app não espera resposta do 0x07 (confirmado no usbmon): fire-and-forget */
    (void)bd_hid_type_a(link, 0x07, 0);
    return true;
}

bool bd_hid_close_session(struct bd_hid_link *link)
{
    (void)bd_hid_type_a(link, 0x07, 1);
    uint8_t leave[BD_REPORT_LEN] = {0x81, 0x05, 0x00, 0x51, 0x02};
    return link_send_raw(link, leave);
}

bool bd_hid_type_a(struct bd_hid_link *link, uint8_t command, uint32_t parameter)
{
    uint8_t payload[BD_HDR_LEN] = {0};
    payload[0] = command;
    memcpy(payload + 4, &parameter, sizeof parameter);

    if (!link_send(link, payload, sizeof payload)) return false;

    for (int attempt = 0; attempt < HID_REPLY_ATTEMPTS; attempt++) {
        uint8_t response[BD_REPORT_LEN];
        if (!link_recv(link, response, HID_REPLY_TIMEOUT_MS)) continue;
        uint32_t echoed = 0;
        memcpy(&echoed, response + 6, sizeof echoed);
        if (response[2] == BD_TAG_ADVANCE && echoed == command) return true;
    }
    return false;
}

bool bd_hid_write_record(struct bd_hid_link *link, const uint8_t *record)
{
    /* crc_value (0x00C) NÃO é recalculado: o device sobrescreve o campo com um
     * contador próprio (1, 2, …), confirmado no hardware (../re/14-write-persist.md). */
    uint32_t offset = 0;
    int consecutive_failures = 0;

    while (offset < BD_REC_LEN) {
        uint32_t remaining = BD_REC_LEN - offset;
        uint16_t chunk = remaining > BD_CHUNK_MAX ? BD_CHUNK_MAX : (uint16_t)remaining;
        if (bd_hid_execute(link, HID_COMMAND_WRITE, 0, BD_REC_LEN, offset, record + offset,
                           chunk, nullptr, 0, nullptr, nullptr)) {
            offset += chunk;
            consecutive_failures = 0;
        } else if (++consecutive_failures > WRITE_MAX_CONSECUTIVE_FAILURES) {
            return false;
        } else {
            usleep(WRITE_RETRY_DELAY_US);
        }
    }

    /* commit Type B (cmd 6 / subcmd 0x15): o device responde ecoando cmd=6 */
    if (!bd_hid_execute(link, HID_COMMAND_COMMIT, HID_COMMIT_SUBCOMMAND, 0, 0,
                        nullptr, 0, nullptr, 0, nullptr, nullptr))
        return false;

    /* O app oficial, após o commit, lê 3× o bloco de 20 bytes (total=20, off=0)
     * e só então relê o record inteiro — visto no usbmon. Replicado aqui. */
    for (int attempt = 0; attempt < 3; attempt++)
        (void)bd_hid_execute(link, HID_COMMAND_READ, 0, 20, 0, nullptr, 20,
                             nullptr, 0, nullptr, nullptr);
    return true;
}
