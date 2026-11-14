/*
 * bd_usb — troca de modo do 8BitDo Ultimate pela interface vendor.
 *
 * Em 2dc8:3106 (XInput) o firmware nao expoe HID, mas aceita o mesmo frame de
 * troca de modo (81 05 00 51 <m>) no endpoint vendor 0x02, o mesmo que o xpad
 * usa para o rumble. Medido no hardware: 32 B nesse endpoint fazem o controle
 * re-enumerar como 2dc8:3012 (DInput, com hidraw) e um segundo frame devolve a
 * XInput. Sem libusb: usbfs direto.
 */
#ifndef BD_USB_H
#define BD_USB_H

#include <stdint.h>

/* Troca o modo do gamepad que estiver em 2dc8:3106.
 * mode: 1 DInput, 2 XInput, 3 DS4, 4 Switch.
 * false = nao achou o controle em XInput ou o envio falhou. */
[[nodiscard]] bool bd_usb_switch_mode(uint8_t mode);

/* True se existe algum 2dc8:3106 no sistema. */
[[nodiscard]] bool bd_usb_has_xinput(void);

#endif /* BD_USB_H */
