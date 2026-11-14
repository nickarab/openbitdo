# 11 — XInput (XUSB): o caminho nativo do 2dc8:3106

## A evidência que resolvia a dúvida

```
/sys/bus/usb/devices/1-3/bNumInterfaces = 1
  1-3:1.0: class=ff sub=5d proto=01 nEndpoints=2 driver=xpad
lsusb -v: bInterfaceClass 255 (Vendor Specific), bInterfaceSubClass 93 (0x5d)
```
**Não existe interface HID nesse modo.** O driver do kernel trata como
`XTYPE_XBOX360`:

```c
{ 0x2dc8, 0x3106, "8BitDo Ultimate Wireless / Pro 2 Wired Controller", 0, XTYPE_XBOX360 },
```
(drivers/input/joystick/xpad.c)

Ou seja: o app oficial usa `HIDManage` **ou** `Xinput` (classes do assembly:
`Advance.Hid.Xinput`, `XINPUT_M`, `xinputModeButton`, `XinputIsConnected`).
Para este controle/modo, é **XUSB**. Insistir em HID estava errado.

## Protocolo (do xpad.c, implementado em `native/xsusb.c`)

| etapa | detalhe |
|---|---|
| init | controle IN `bmRequestType=0xC1`, `bRequest=0x01`, `wValue=0x100`, `wIndex=0`, 20 B, 25 ms (alguns pads de terceiros exigem; aqui devolve `-EPIPE` e funciona sem) |
| input | interrupt IN **EP 0x81**, **20 bytes**, `data[0] == 0x00` |
| rumble | interrupt OUT **EP 0x02**, **8 bytes**: `00 08 00 <strong>>8> <weak>>8> 00 00 00` |

Layout do report (xpad360_process_packet):

```
byte 2: bit0 UP  bit1 DOWN  bit2 LEFT bit3 RIGHT
        bit4 START bit5 BACK bit6 L3  bit7 R3
byte 3: bit0 LB  bit1 RB  bit2 HOME
        bit4 A   bit5 B   bit6 X   bit7 Y
byte 4: gatilho esquerdo (0..255)      byte 5: gatilho direito
byte 6..7  LS X (int16 LE)             byte 8..9   LS Y (invertido: ~v)
byte 10..11 RS X                       byte 12..13 RS Y (invertido)
```

Captura real do device (primeiro report):

```
00 14 00 00 00 00 3e ff c0 00 00 00 00 00 00 00 00 00 00 00
   ^  ^                     ^^^^^ LS X = -194, LS Y = 0xc0ff...
```

## Permissão (udev) — necessária

`/etc/udev/rules.d/72-8bitdo.rules`:

```
SUBSYSTEM=="usb",    ATTRS{idVendor}=="2dc8", TAG+="uaccess"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2dc8", TAG+="uaccess"
SUBSYSTEM=="input",  ATTRS{idVendor}=="2dc8", TAG+="uaccess"
```

## O que funciona hoje

- `xsusb --watch` — estado ao vivo no terminal (botões, d-pad, sticks, gatilhos).
- `xsusb --rumble L R` — vibração (L/R até 65535).
- No app (`8bdgui`): aba **XInput (USB)** com estado ao vivo, barras de vibração e
  **destaque dos botões sobre a arte** do controle; auto-conexão ao detectar o 0x3106.

Importante: ao conectar, o app **toma a interface do `xpad`** (é o mesmo que o app
oficial faz — o rodapé dele diz "*The controller and the profile will not work until
the software is closed*"). O botão *Liberar XInput* devolve o pad ao sistema.

## O que NÃO existe por XUSB (limite honesto)

O record de perfil de **1652 bytes** (mapeamentos, sticks, vibração, macros) usa o
protocolo vendor HID (`0xFFA0`, reports `0x02`/`0x81`) — esse continua sendo o canal
de configuração, disponível quando o controle está no modo software/dongle.
As extensões vendor do 8BitDo sobre XUSB (`_usbrr_changeXinputMode`,
`_changeUSBPlatform`) não foram decodificadas: exigiriam captura de tráfego USB do app
oficial rodando no Windows.
