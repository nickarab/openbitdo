/*
 * test_hid.c — testes do transporte bd_hid sem hardware.
 *
 * Um socketpair(AF_UNIX, SOCK_SEQPACKET) faz o papel do hidraw: cada write()
 * é um report de 64 B com fronteira preservada, como no /dev/hidraw*. O lado
 * "device" pré-enfileira as respostas e depois confere os frames recebidos.
 * Build: make tests (na raiz) → build/test_hid
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bd_core.h"
#include "bd_hid.h"

static int fails, checks;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        checks++;                                                   \
        if (!(cond)) {                                              \
            fails++;                                                \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                              \
    do {                                                            \
        checks++;                                                   \
        long long _a = (long long)(a), _b = (long long)(b);          \
        if (_a != _b) {                                             \
            fails++;                                                \
            printf("  FAIL %s:%d: obtido=%lld esperado=%lld\n",     \
                   __FILE__, __LINE__, _a, _b);                     \
        }                                                           \
    } while (0)

/* Payload Tipo C dentro do frame 81/len/04: header em +3, dados em +19. */
constexpr size_t FRAME_PAYLOAD = 3;
constexpr size_t FRAME_DATA = FRAME_PAYLOAD + BD_HDR_LEN;
constexpr size_t MAX_FRAMES = 64;

struct fake {
    struct bd_hid_link link;  /* lado do host, como bd_hid_open deixaria */
    int device;               /* lado do controle */
};

static bool fake_open(struct fake *fake, uint16_t pid)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0)
        return false;
    const int flags = fcntl(pair[0], F_GETFL);
    (void)fcntl(pair[0], F_SETFL, flags | O_NONBLOCK);
    *fake = (struct fake){.link = {.fd = pair[0], .pid = pid}, .device = pair[1]};
    return true;
}

static void fake_close(struct fake *fake)
{
    bd_hid_close(&fake->link);
    close(fake->device);
}

/* Enfileira a resposta "02 04 .. sub@4 len@6 dados@18" que o host vai ler. */
static void fake_reply(const struct fake *fake, uint16_t sub, uint16_t length,
                       const uint8_t *data)
{
    uint8_t report[BD_REPORT_LEN] = {};
    report[0] = BD_REPORT_IN;
    report[1] = BD_TAG_ADVANCE;
    bd_wr16(report, 4, sub);
    bd_wr16(report, 6, length);
    if (data != nullptr) {
        const size_t room = BD_REPORT_LEN - 18;
        memcpy(report + 18, data, length < room ? length : room);
    }
    (void)!write(fake->device, report, sizeof report);
}

/* Drena o que o host escreveu; devolve quantos frames e quantos não tinham 64 B. */
static size_t fake_drain(const struct fake *fake, uint8_t frames[][BD_REPORT_LEN],
                         size_t capacity, size_t *wrong_size)
{
    size_t count = 0;
    *wrong_size = 0;
    uint8_t buffer[BD_REPORT_LEN * 2];
    ssize_t got;
    while ((got = recv(fake->device, buffer, sizeof buffer, MSG_DONTWAIT)) > 0) {
        if (got != BD_REPORT_LEN)
            ++*wrong_size;
        if (count < capacity)
            memcpy(frames[count], buffer, BD_REPORT_LEN);
        ++count;
    }
    return count;
}

static uint16_t frame_u16(const uint8_t frame[BD_REPORT_LEN], size_t at)
{
    return bd_rd16(frame, FRAME_PAYLOAD + at);
}

static uint32_t frame_u32(const uint8_t frame[BD_REPORT_LEN], size_t at)
{
    return bd_rd32(frame, FRAME_PAYLOAD + at);
}

static uint8_t frames[MAX_FRAMES][BD_REPORT_LEN];

/* ----------------------------------------------------- frames de controle -- */
static void test_raw_frames(void)
{
    printf("[frames crus]\n");
    struct fake fake;
    if (!fake_open(&fake, 0x3012)) { CHECK(false, "socketpair"); return; }
    size_t wrong = 0;

    CHECK(bd_hid_mode_switch(&fake.link, 1), "mode_switch envia");
    CHECK_EQ(fake_drain(&fake, frames, MAX_FRAMES, &wrong), 1);
    CHECK_EQ(wrong, 0);
    const uint8_t mode_dinput[5] = {0x81, 0x05, 0x00, 0x51, 0x01};
    CHECK(memcmp(frames[0], mode_dinput, sizeof mode_dinput) == 0, "81 05 00 51 01");
    bool padded = true;
    for (size_t i = sizeof mode_dinput; i < BD_REPORT_LEN; i++)
        padded = padded && frames[0][i] == 0;
    CHECK(padded, "resto do changeDinput_CMD zerado até 64 B");
    CHECK(bd_hid_mode_switch(&fake.link, 4), "mode_switch Switch");
    CHECK_EQ(fake_drain(&fake, frames, MAX_FRAMES, &wrong), 1);
    CHECK_EQ(frames[0][4], 4);                            /* o modo vai no byte 4 */

    /* Sessão como está hoje (a semântica será revista com hardware):
     * abrir = 81 05 00 21 01 + Type A 0x07/0; fechar = Type A 0x07/1 + 81 05 00 51 02. */
    CHECK(bd_hid_open_session(&fake.link), "open_session não depende de ACK");
    CHECK_EQ(fake_drain(&fake, frames, MAX_FRAMES, &wrong), 2);
    const uint8_t enter[5] = {0x81, 0x05, 0x00, 0x21, 0x01};
    CHECK(memcmp(frames[0], enter, sizeof enter) == 0, "abre com 81 05 00 21 01");
    CHECK_EQ(frames[1][3], 0x07);
    CHECK_EQ(frame_u32(frames[1], 4), 0);

    CHECK(bd_hid_close_session(&fake.link), "close_session envia");
    CHECK_EQ(fake_drain(&fake, frames, MAX_FRAMES, &wrong), 2);
    CHECK_EQ(frames[0][3], 0x07);
    CHECK_EQ(frame_u32(frames[0], 4), 1);
    const uint8_t leave[5] = {0x81, 0x05, 0x00, 0x51, 0x02};
    CHECK(memcmp(frames[1], leave, sizeof leave) == 0, "fecha com 81 05 00 51 02");
    fake_close(&fake);
}

/* ------------------------------------------------------------- Type A ----- */
static void test_type_a(void)
{
    printf("[Type A]\n");
    struct fake fake;
    if (!fake_open(&fake, 0x3012)) { CHECK(false, "socketpair"); return; }
    size_t wrong = 0;

    CHECK(!bd_hid_type_a(&fake.link, 0x09, 0x01020304), "sem resposta = false");
    CHECK_EQ(fake_drain(&fake, frames, MAX_FRAMES, &wrong), 1);
    CHECK_EQ(frames[0][0], BD_REPORT_OUT);
    CHECK_EQ(frames[0][1], BD_HDR_LEN + 1);
    CHECK_EQ(frames[0][2], BD_TAG_ADVANCE);
    CHECK_EQ(frames[0][3], 0x09);
    CHECK_EQ(frames[0][4], 0);
    CHECK_EQ(frame_u32(frames[0], 4), 0x01020304);       /* param u32 LE em +4 */
    fake_close(&fake);
}

/* ------------------------------------------------------------- Type C ----- */
static void test_execute(void)
{
    printf("[Type C execute]\n");
    struct fake fake;
    if (!fake_open(&fake, 0x3012)) { CHECK(false, "socketpair"); return; }
    size_t wrong = 0;

    /* eco de outro comando é ignorado; o eco certo logo depois é aceito */
    uint8_t data[60];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(0x40 + i);
    fake_reply(&fake, 0x0009, 4, data);
    fake_reply(&fake, 0x0002, 60, data);
    uint8_t out[64] = {};
    int out_length = -1;
    volatile int progress = -1;
    CHECK(bd_hid_execute(&fake.link, 0x0002, 0, BD_REC_LEN, 90, nullptr, 45,
                         out, (int)sizeof out, &out_length, &progress),
          "aceita o eco do mesmo cmd");
    CHECK_EQ(out_length, BD_REPORT_LEN - 18);             /* len 60 limitado ao report */
    CHECK(memcmp(out, data, (size_t)out_length) == 0, "dados copiados de +18");
    CHECK_EQ(progress, 90);

    CHECK_EQ(fake_drain(&fake, frames, MAX_FRAMES, &wrong), 1);
    CHECK_EQ(wrong, 0);
    CHECK_EQ(frames[0][1], BD_HDR_LEN + 1);               /* read: só o header */
    CHECK_EQ(frame_u16(frames[0], 0), 0x0002);
    CHECK_EQ(frame_u16(frames[0], 4), 45);
    CHECK_EQ(frame_u16(frames[0], 6), 0);                 /* 0x3012 não usa CRC */
    CHECK_EQ(frame_u32(frames[0], 8), BD_REC_LEN);
    CHECK_EQ(frame_u32(frames[0], 12), 90);

    /* out_capacity menor que a resposta trunca no buffer do chamador */
    fake_reply(&fake, 0x0002, 30, data);
    uint8_t small[10] = {};
    CHECK(bd_hid_execute(&fake.link, 0x0002, 0, 20, 0, nullptr, 20, small,
                         (int)sizeof small, &out_length, nullptr), "resposta truncada");
    CHECK_EQ(out_length, 10);
    (void)fake_drain(&fake, frames, MAX_FRAMES, &wrong);

    /* sem eco nenhum: false depois das tentativas */
    CHECK(!bd_hid_execute(&fake.link, 0x0001, 0, 0, 0, data, 8, nullptr, 0, nullptr, nullptr),
          "device mudo = false");
    CHECK_EQ(fake_drain(&fake, frames, MAX_FRAMES, &wrong), 1);
    CHECK_EQ(frames[0][1], BD_HDR_LEN + 8 + 1);
    CHECK(memcmp(frames[0] + FRAME_DATA, data, 8) == 0, "dados da escrita em +19");

    fake_close(&fake);

    /* PIDs da tabela de CRC levam o CRC dos dados no header */
    if (!fake_open(&fake, 0x3109)) { CHECK(false, "socketpair"); return; }
    fake_reply(&fake, 0x0001, 0, nullptr);
    CHECK(bd_hid_execute(&fake.link, 0x0001, 0, BD_REC_LEN, 0, data, 8, nullptr, 0,
                         nullptr, nullptr), "write com CRC");
    CHECK_EQ(fake_drain(&fake, frames, MAX_FRAMES, &wrong), 1);
    CHECK_EQ(frame_u16(frames[0], 6), bd_crc16(data, 8));
    fake_close(&fake);
}

/* ---------------------------------------------------- record inteiro ------ */
static void fill_pattern(uint8_t record[BD_REC_LEN])
{
    for (size_t i = 0; i < BD_REC_LEN; i++)
        record[i] = (uint8_t)((i * 31u + 7u) & 0xFFu);
}

static void test_read_record(void)
{
    printf("[read_record]\n");
    struct fake fake;
    if (!fake_open(&fake, 0x3012)) { CHECK(false, "socketpair"); return; }
    size_t wrong = 0;

    uint8_t expected[BD_REC_LEN];
    fill_pattern(expected);
    for (uint32_t offset = 0; offset < BD_REC_LEN; offset += bd_chunk_len(offset))
        fake_reply(&fake, 0x0002, bd_chunk_len(offset), expected + offset);

    uint8_t record[BD_REC_LEN] = {};
    volatile int progress = 0;
    CHECK(bd_hid_read_record(&fake.link, record, &progress), "lê os 37 chunks");
    CHECK(memcmp(record, expected, sizeof record) == 0, "record igual ao do device");
    CHECK_EQ(progress, BD_REC_LEN);

    const size_t sent = fake_drain(&fake, frames, MAX_FRAMES, &wrong);
    CHECK_EQ(sent, 37);
    CHECK_EQ(wrong, 0);
    uint32_t offset = 0;
    for (size_t i = 0; i < sent && i < MAX_FRAMES; i++) {
        CHECK_EQ(frame_u16(frames[i], 0), 0x0002);
        CHECK_EQ(frame_u16(frames[i], 4), bd_chunk_len(offset));
        CHECK_EQ(frame_u32(frames[i], 12), offset);
        offset += bd_chunk_len(offset);
    }

    /* device mudo no primeiro chunk: desiste em 2 tentativas, sem insistir */
    CHECK(!bd_hid_read_record(&fake.link, record, nullptr), "device mudo = false");
    CHECK_EQ(fake_drain(&fake, frames, MAX_FRAMES, &wrong), 2);
    fake_close(&fake);
}

static void test_write_record(void)
{
    printf("[write_record]\n");
    struct fake fake;
    if (!fake_open(&fake, 0x3012)) { CHECK(false, "socketpair"); return; }
    size_t wrong = 0;

    uint8_t record[BD_REC_LEN];
    fill_pattern(record);
    for (int i = 0; i < 37; i++) fake_reply(&fake, 0x0001, 0, nullptr);
    fake_reply(&fake, 0x0006, 0, nullptr);
    for (int i = 0; i < 3; i++) fake_reply(&fake, 0x0002, 20, record);

    CHECK(bd_hid_write_record(&fake.link, record), "37 chunks + commit confirmados");
    const size_t sent = fake_drain(&fake, frames, MAX_FRAMES, &wrong);
    CHECK_EQ(sent, 37 + 1 + 3);
    CHECK_EQ(wrong, 0);

    uint32_t offset = 0;
    for (size_t i = 0; i < 37 && i < sent; i++) {
        const uint16_t chunk = bd_chunk_len(offset);
        CHECK_EQ(frame_u16(frames[i], 0), 0x0001);
        CHECK_EQ(frame_u16(frames[i], 4), chunk);
        CHECK_EQ(frame_u32(frames[i], 8), BD_REC_LEN);
        CHECK_EQ(frame_u32(frames[i], 12), offset);
        CHECK(memcmp(frames[i] + FRAME_DATA, record + offset, chunk) == 0,
              "dados do chunk @%u", (unsigned)offset);
        offset += chunk;
    }
    if (sent > 37) {
        CHECK_EQ(frame_u16(frames[37], 0), 0x0006);       /* commit */
        CHECK_EQ(frame_u16(frames[37], 2), 0x0015);
        CHECK_EQ(frame_u16(frames[37], 4), 0);
    }
    if (sent > 38) {
        CHECK_EQ(frame_u16(frames[38], 0), 0x0002);       /* releitura de 20 B */
        CHECK_EQ(frame_u32(frames[38], 8), 20);
    }

    /* commit sem eco: a gravação falha */
    for (int i = 0; i < 37; i++) fake_reply(&fake, 0x0001, 0, nullptr);
    CHECK(!bd_hid_write_record(&fake.link, record), "commit sem ACK = false");
    (void)fake_drain(&fake, frames, MAX_FRAMES, &wrong);
    fake_close(&fake);
}

int main(void)
{
    printf("bd_hid — testes de transporte (socketpair)\n");
    test_raw_frames();
    test_type_a();
    test_execute();
    test_read_record();
    test_write_record();
    printf("\n%d checagens, %d falha(s)\n", checks, fails);
    return fails ? 1 : 0;
}
