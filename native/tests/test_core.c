/*
 * test_core.c — suíte de regressão do núcleo (sem GTK, sem hardware)
 * Build: make tests (na raiz) → build/test_core
 */
#include <stdio.h>
#include <string.h>

#include "../bd_core.h"

/* Contrato de tamanho verificado em tempo de compilação: se um offset ou
 * constante do protocolo mudar, o build quebra antes de rodar os testes. */
static_assert(BD_REC_LEN == 0x674, "record do perfil tem 1652 bytes");
static_assert(BD_REPORT_LEN == 64, "report HID tem 64 bytes");
static_assert(BD_HDR_LEN == 16, "header Tipo C tem 16 bytes");
static_assert(BD_NKEYS == 20, "20 teclas mapeadas");

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

#define CHECK_EQ(a, b)                                             \
    do {                                                            \
        checks++;                                                   \
        long long _a = (long long)(a), _b = (long long)(b);          \
        if (_a != _b) {                                             \
            fails++;                                                \
            printf("  FAIL %s:%d: obtido=%lld esperado=%lld\n",     \
                   __FILE__, __LINE__, _a, _b);                     \
        }                                                           \
    } while (0)

/* ------------------------ 1b. plano de chunks (1652 = 36×45 + 32) ------- */
static void test_chunk_plan(void)
{
    printf("[plano de chunks]\n");
    constexpr uint32_t full_chunks = 36;
    constexpr uint16_t last_chunk_bytes = 32;

    uint32_t offset = 0;
    uint32_t index = 0;
    for (uint16_t chunk; (chunk = bd_chunk_len(offset)) != 0; offset += chunk, ++index) {
        CHECK_EQ(chunk, index < full_chunks ? BD_CHUNK_MAX : last_chunk_bytes);
        CHECK(index <= full_chunks, "chunk extra em offset %u", (unsigned)offset);
    }
    CHECK_EQ(index, full_chunks + 1);
    CHECK_EQ(offset, BD_REC_LEN);
    CHECK_EQ(bd_chunk_len(BD_REC_LEN - 1), 1);
    CHECK_EQ(bd_chunk_len(BD_REC_LEN), 0);
    CHECK_EQ(bd_chunk_len(UINT32_MAX), 0);
}

/* ------------------------------------------------ 2. CRC (vetor externo) -- */
static void test_crc(void)
{
    printf("[crc16]\n");
    /* "123456789": CRC-16/X-25 = 0x906E (init 0xFFFF, refl, xorout 0xFFFF).
     * Nossa variante é a mesma sem xorout e com bytes trocados no fim:
     *   raw = 0x906E ^ 0xFFFF = 0x6F91 ; swap = 0x916F                       */
    const char *v = "123456789";
    CHECK_EQ(bd_crc16((const uint8_t *)v, 9), 0x916F);

    /* payload vazio -> 0xFFFF (o controle ecoou exatamente isso no Type C) */
    CHECK_EQ(bd_crc16(nullptr, 0), 0xFFFF);

    /* independência do buffer: 8 bytes de 0xFF não é 0xFFFF */
    uint8_t ff[8];
    memset(ff, 0xff, sizeof ff);
    CHECK(bd_crc16(ff, sizeof ff) != 0xFFFF, "crc de 8x0xFF não deveria ser 0xFFFF");

    /* lista de PIDs com CRC no Type C */
    CHECK(bd_pid_uses_crc(0x3109) == 1, "0x3109 (dongle) usa CRC");
    CHECK(bd_pid_uses_crc(0x3011) == 1, "0x3011 usa CRC");
    CHECK(bd_pid_uses_crc(0x3012) == 0, "0x3012 (Ultimate 2.4) NÃO usa CRC");
    CHECK(bd_pid_uses_crc(0x3106) == 0, "0x3106 (XInput) NÃO usa CRC");
}

/* ------------------------------------------------------ 3. acesso a campos -- */
static void test_record_fields(void)
{
    printf("[record fields]\n");
    uint8_t rec[BD_REC_LEN];
    memset(rec, 0, sizeof rec);

    bd_wr16(rec, BD_OFF_MODE, 3);
    bd_wr16(rec, BD_OFF_SLOT, 1);
    CHECK_EQ(rec[BD_OFF_MODE], 3);
    CHECK_EQ(rec[BD_OFF_MODE + 1], 0);
    CHECK_EQ(rec[BD_OFF_SLOT], 1);
    CHECK_EQ(bd_rd16(rec, BD_OFF_MODE), 3);
    CHECK_EQ(bd_rd16(rec, BD_OFF_SLOT), 1);

    bd_wr32(rec, BD_OFF_CRC, 0xdeadbeef);
    CHECK_EQ(bd_rd32(rec, BD_OFF_CRC), 0xdeadbeef);
    CHECK_EQ(rec[BD_OFF_CRC], 0xef);                   /* little-endian */

    /* mapeamento: chave 0 do slot 0 fica em OFF_MAP + 4 (após o flag) */
    bd_wr32(rec, BD_OFF_MAP + 4 + 0 * 4, 0x2000);            /* A */
    bd_wr32(rec, BD_OFF_MAP + 4 + 18 * 4, 0x2000000);        /* P1 */
    bd_wr32(rec, BD_OFF_MAP + 4 + 19 * 4, 0x4000000);        /* P2 */
    CHECK_EQ(bd_rd32(rec, BD_OFF_MAP + 4), 0x2000);
    CHECK_EQ(bd_rd32(rec, BD_OFF_MAP + 4 + 18 * 4), 0x2000000);
    CHECK_EQ(bd_rd32(rec, BD_OFF_MAP + 4 + 19 * 4), 0x4000000);

    /* slot 1 começa 84 bytes depois */
    bd_wr32(rec, BD_OFF_MAP + 84 + 4, 0x1000);
    CHECK_EQ(bd_rd32(rec, BD_OFF_MAP + 84 + 4), 0x1000);

    /* stick esquerdo / gatilho esquerdo do slot 0 */
    rec[BD_OFF_JOY + 4] = 200;
    rec[BD_OFF_TRG + 4] = 100;
    CHECK_EQ(rec[BD_OFF_JOY + 4], 200);
    CHECK_EQ(rec[BD_OFF_TRG + 4], 100);
}

/* ------------------------------------------------------------- 4. nomes --- */
static void test_names(void)
{
    printf("[nomes UTF-16LE]\n");
    uint8_t rec[BD_REC_LEN];
    memset(rec, 0, sizeof rec);
    char out[64];

    bd_name_set(rec, 0, "Profile 1");
    bd_name_get(rec, 0, out, sizeof out);
    CHECK(strcmp(out, "Profile 1") == 0, "roundtrip simples: '%s'", out);
    CHECK_EQ(rec[BD_OFF_NAME], 'P');
    CHECK_EQ(rec[BD_OFF_NAME + 1], 0);                  /* UTF-16LE */

    bd_name_set(rec, 1, "Ação");
    bd_name_get(rec, 1, out, sizeof out);
    CHECK(strcmp(out, "Ação") == 0, "acentos: '%s'", out);

    bd_name_set(rec, 2, "0123456789ABCDEFGHIJ");              /* 20 chars */
    bd_name_get(rec, 2, out, sizeof out);
    CHECK(strlen(out) == 15, "truncado em 15 code units (obtido %zu)", strlen(out));
    CHECK(rec[BD_OFF_NAME + 2 * 32 + 30] == 0 && rec[BD_OFF_NAME + 2 * 32 + 31] == 0,
          "resto do campo zerado");

    /* 15 code units cabem; o NUL cai no 16º unit do campo */
    bd_name_set(rec, 0, "ABCDEFGHIJKLMNO");
    bd_name_get(rec, 0, out, sizeof out);
    CHECK(strcmp(out, "ABCDEFGHIJKLMNO") == 0, "15 code units: '%s'", out);
    CHECK_EQ(rec[BD_OFF_NAME + 14 * 2], 'O');
    CHECK_EQ(rec[BD_OFF_NAME + 15 * 2], 0);

    bd_name_set(rec, 0, "ABCDEFGHIJKLMNOP");
    bd_name_get(rec, 0, out, sizeof out);
    CHECK(strcmp(out, "ABCDEFGHIJKLMNO") == 0, "16ª unidade descartada: '%s'", out);
    CHECK_EQ(rec[BD_OFF_NAME + 15 * 2], 0);

    /* U+1F600 não cabe no UTF-16 de 16 bits do record; o codec o descarta
     * sem escrever fora dos 32 bytes do slot. */
    uint8_t guarded[BD_REC_LEN + 32];
    memset(guarded, 0xa5, sizeof guarded);
    bd_name_set(guarded, 2, "ABCDEFGHIJKLMN\U0001F600O");
    bd_name_get(guarded, 2, out, sizeof out);
    CHECK(strcmp(out, "ABCDEFGHIJKLMNO") == 0, "fora do BMP descartado: '%s'", out);
    const size_t slot_begin = (size_t)BD_OFF_NAME + 2 * 32;
    int outside_intact = 1;
    for (size_t index = 0; index < sizeof guarded; ++index) {
        if (index >= slot_begin && index < slot_begin + 32)
            continue;
        if (guarded[index] != 0xa5)
            outside_intact = 0;
    }
    CHECK(outside_intact, "escrita do nome fica dentro do slot de 32 bytes");
    CHECK_EQ(guarded[slot_begin + (BD_NAME_UNITS - 1) * 2], 0);
    CHECK_EQ(guarded[slot_begin + (BD_NAME_UNITS - 1) * 2 + 1], 0);

    bd_name_set(rec, 0, "");
    bd_name_get(rec, 0, out, sizeof out);
    CHECK(out[0] == 0, "nome vazio");

    /* não vaza entre slots (32 bytes cada) */
    bd_name_set(rec, 0, "AAA");
    bd_name_set(rec, 1, "BBB");
    bd_name_get(rec, 0, out, sizeof out);
    CHECK(strcmp(out, "AAA") == 0, "slot 0 intacto após escrever slot 1: '%s'", out);
}

/* ------------------------------------------------------- 5. tabelas/mapeamento */
static void test_tables(void)
{
    printf("[tabelas]\n");
    CHECK_EQ(bd_actions_count, 26);
    CHECK_EQ(bd_action_index(0x2000), 1);               /* A */
    CHECK_EQ(bd_action_index(0x4000000), 24);           /* P2 */
    CHECK_EQ(bd_action_index(0xdead), 0);               /* desconhecido -> 0 */

    /* 20 chaves: 18 do enum + P1/P2 no fim */
    CHECK(strcmp(bd_key_short[18], "P1") == 0, "indice 18 = P1 ('%s')", bd_key_short[18]);
    CHECK(strcmp(bd_key_short[19], "P2") == 0, "indice 19 = P2 ('%s')", bd_key_short[19]);
    CHECK(strcmp(bd_key_short[16], "D_LEFT") == 0, "indice 16 = D_LEFT ('%s')", bd_key_short[16]);
    CHECK(strcmp(bd_key_short[17], "D_RIGHT") == 0, "indice 17 = D_RIGHT ('%s')",
          bd_key_short[17]);
    CHECK(strstr(bd_key_long[17], "d-pad") != nullptr, "d-pad right rotulado como d-pad");
    /* o controle e layout Xbox: L1/R1/L2/R2/L3/R3 do vocabulario 8BitDo apontam
     * para LB/RB/LT/RT/LS/RS, e RT e o gatilho (nao o d-pad). */
    CHECK_EQ(bd_action_value("L1"), 0x400);
    CHECK_EQ(bd_action_value("R2"), 0x8000);
    CHECK_EQ(bd_action_value("RT"), 0x8000);
    CHECK_EQ(bd_action_value("D_RIGHT"), 0x40);
    CHECK_EQ(bd_action_value("VIEW"), 0x8);
    CHECK_EQ(bd_action_value("START"), 0x1);
    CHECK_EQ(bd_action_value("a"), 0x2000);               /* sem diferenciar caixa */
    CHECK_EQ(bd_action_value("select"), 0x8);             /* alias em minúsculas */
    CHECK_EQ(bd_action_value("DN"), 0x100);
    CHECK_EQ(bd_action_value("P2"), 0x4000000);
    CHECK_EQ(bd_action_value("NAOEXISTE"), 0);
    CHECK_EQ(bd_action_value(""), 0);
    for (size_t i = 1; i < bd_actions_count; i++)
        CHECK_EQ(bd_action_index(bd_action_value(bd_actions[i].name)), (long long)i);
    CHECK(strcmp(bd_mode_names[3], "XInput") == 0, "modo 3 = XInput");
    CHECK(strcmp(bd_mode_names[1], "DInput") == 0, "modo 1 = DInput");
}

/* --------------------------------------------------------------- 6. HID --- */
static void test_hid(void)
{
    printf("[framing HID]\n");
    uint8_t pl[BD_HDR_LEN];
    memset(pl, 0, sizeof pl);
    bd_wr16(pl, 0, 2);                       /* cmd read */
    bd_wr16(pl, 4, 20);                      /* len */
    bd_wr32(pl, 8, BD_REC_LEN);              /* total */
    uint8_t fr[BD_REPORT_LEN];
    size_t n = bd_hid_build_frame(pl, sizeof pl, fr);
    CHECK_EQ(n, 19);
    CHECK_EQ(fr[0], 0x81);
    CHECK_EQ(fr[1], 17);               /* len+1 */
    CHECK_EQ(fr[2], 0x04);
    CHECK_EQ(bd_rd16(fr + 3, 0), 2);
    CHECK_EQ(bd_rd32(fr + 3, 8), BD_REC_LEN);
    for (int i = 19; i < BD_REPORT_LEN; i++)
        CHECK(fr[i] == 0, "padding zero em %d", i);

    /* payload vazio: só o cabeçalho; nullptr é aceito quando len == 0 */
    CHECK_EQ(bd_hid_build_frame(nullptr, 0, fr), 3);
    CHECK_EQ(fr[1], 1);
    CHECK(fr[2] == BD_TAG_ADVANCE, "tag 0x04 no frame vazio");

    /* 61 bytes de payload = máximo (len+3 = 64) */
    uint8_t big[64];
    memset(big, 0xaa, sizeof big);
    CHECK_EQ(bd_hid_build_frame(big, 61, fr), 64);
    CHECK_EQ(fr[1], 62);
    CHECK_EQ(bd_hid_build_frame(big, 62, fr), 0);     /* 62+3 > 64 -> rejeita */

    /* parse de resposta */
    uint8_t r[BD_REPORT_LEN];
    memset(r, 0, sizeof r);
    r[0] = 0x02; r[1] = 0x04;
    bd_wr16(r, 4, 2);          /* subcmd echo */
    bd_wr16(r, 6, 20);         /* len */
    r[18] = 0xAB;
    uint16_t sub = 0, len = 0;
    const uint8_t *data = nullptr;
    CHECK(bd_hid_parse_resp(r, sizeof r, &sub, &len, &data) == 1, "resposta válida");
    CHECK_EQ(sub, 2);
    CHECK_EQ(len, 20);
    CHECK(data && data[0] == 0xAB, "payload em +18");

    r[0] = 0x81;
    CHECK(bd_hid_parse_resp(r, sizeof r, nullptr, nullptr, nullptr) == 0,
          "report id errado rejeitado");
    r[0] = 0x02;
    CHECK(bd_hid_parse_resp(r, 10, nullptr, nullptr, nullptr) == 0,
          "report curto rejeitado");

    /* header Tipo C: cmd/sub/len/crc em 0/2/4/6 e total/offset em 8/12 */
    uint8_t hdr[BD_HDR_LEN];
    memset(hdr, 0xa5, sizeof hdr);
    bd_type_c_header(0x1234, 0xabcd, 0x002d, 0xef01, 0x11223344, 0x55667788, hdr);
    const uint8_t hdr_le[BD_HDR_LEN] = {
        0x34, 0x12, 0xcd, 0xab, 0x2d, 0x00, 0x01, 0xef,
        0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
    };
    CHECK(memcmp(hdr, hdr_le, sizeof hdr_le) == 0, "header Tipo C little-endian");
    CHECK_EQ(bd_rd16(hdr, 0), 0x1234);
    CHECK_EQ(bd_rd16(hdr, 2), 0xabcd);
    CHECK_EQ(bd_rd16(hdr, 4), 0x002d);
    CHECK_EQ(bd_rd16(hdr, 6), 0xef01);
    CHECK_EQ(bd_rd32(hdr, 8), 0x11223344);
    CHECK_EQ(bd_rd32(hdr, 12), 0x55667788);
}

int main(void)
{
    printf("8bdgui — testes de núcleo\n");
    test_chunk_plan();
    test_crc();
    test_record_fields();
    test_names();
    test_tables();
    test_hid();
    printf("\n%d checagens, %d falha(s)\n", checks, fails);
    return fails ? 1 : 0;
}
