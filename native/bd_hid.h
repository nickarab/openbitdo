/*
 * bd_hid — transporte HID do 8BitDo Ultimate (vendor reports 0x81 out / 0x02 in).
 *
 * Header Tipo C de 16 B: cmd u16 | subcmd u16 | len u16 | crc u16 | total u32 | offset u32.
 * O crc do header só é enviado nos PIDs de bd_pid_uses_crc(); o crc_value (0x00C) do
 * record é contador do próprio device e não se calcula aqui.
 */
#ifndef BD_HID_H
#define BD_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "bd_core.h"

#define BD_HID_MAX_DEVICES 32
#define BD_HID_PATH_SIZE 64

struct bd_hid_device {
    char path[BD_HID_PATH_SIZE];
    uint16_t pid;
};

struct bd_hid_link {
    int fd;
    uint16_t pid;
};

/* Enumera /sys/class/hidraw com VID 0x2dc8, preenchendo até capacity entradas.
 * Retorna quantas achou, ou -1 se /sys/class/hidraw não abrir. */
[[nodiscard]] int bd_hid_find(struct bd_hid_device *devices, int capacity);

/* Abre em O_NONBLOCK: com o device sem drenar, write() bloquearia para sempre.
 * Em erro retorna false e link->fd fica -1. */
[[nodiscard]] bool bd_hid_open(struct bd_hid_link *link, const char *path, uint16_t pid);

/* Fecha e deixa fd = -1. Link que nunca foi aberto precisa de fd == -1: um link
 * zerado tem fd 0 e fecharia stdin. */
void bd_hid_close(struct bd_hid_link *link);

/* Envia um comando Type C e espera o eco do mesmo cmd. data nullptr manda só o header
 * (o read leva o tamanho no campo len). out, out_length e progress são opcionais;
 * progress recebe o offset concluído. false = envio falhou ou o device não ecoou. */
[[nodiscard]] bool bd_hid_execute(struct bd_hid_link *link, uint16_t command, uint16_t subcommand,
                                  uint32_t total, uint32_t offset, const uint8_t *data,
                                  uint16_t length, uint8_t *out, int out_capacity,
                                  int *out_length, volatile int *progress);

/* Sessão de configuração (medida no usbmon):
 *   abre : frame cru "81 05 00 21 01" + Type A cmd 0x07 param 0
 *   fecha: Type A cmd 0x07 param 1 + frame cru "81 05 00 51 02"
 * O device só aplica o perfil gravado dentro da sessão; ao fechar ele volta ao
 * modo normal (gamepad volta a ser reconhecido pelo PC). */
[[nodiscard]] bool bd_hid_open_session(struct bd_hid_link *link);
[[nodiscard]] bool bd_hid_close_session(struct bd_hid_link *link);

/* Frame cru 81 05 00 51 <mode>: troca o modo do gamepad (1 DInput, 2 XInput,
 * 3 DS4, 4 Switch). E como o app oficial devolve o controle ao XInput. */
[[nodiscard]] bool bd_hid_mode_switch(struct bd_hid_link *link, uint8_t mode);

/* Comando Type A (byte de comando + u32 no offset 4): 0x09 seleciona o slot,
 * 0x0A/0x0F trocam de plataforma. true = device ecoou o comando. */
[[nodiscard]] bool bd_hid_type_a(struct bd_hid_link *link, uint8_t command,
                                uint32_t parameter);

/* Lê os BD_REC_LEN (0x674) bytes do record de perfil. false = leitura incompleta. */
[[nodiscard]] bool bd_hid_read_record(struct bd_hid_link *link, uint8_t *record,
                                      volatile int *progress);

/* Grava o record em chunks de 45 B, cada um confirmado pelo device, e fecha com o
 * commit cmd 6 / subcmd 0x15. false = parou de confirmar um chunk ou o commit falhou. */
[[nodiscard]] bool bd_hid_write_record(struct bd_hid_link *link, const uint8_t *record);

#endif /* BD_HID_H */
