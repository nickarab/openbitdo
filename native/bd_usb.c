#define _GNU_SOURCE

#include "bd_usb.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define BD_VID "2dc8"
#define BD_PID_XINPUT "3106"
#define BD_VENDOR_ENDPOINT 0x02
#define BD_MODE_FRAME_LEN 32

static bool read_attr(const char *device, const char *attribute, char *out, size_t capacity)
{
    char path[256];
    const int written = snprintf(path, sizeof path, "/sys/bus/usb/devices/%s/%s", device, attribute);
    if (written < 0 || (size_t)written >= sizeof path) return false;

    const int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    const ssize_t got = read(fd, out, capacity - 1);
    close(fd);
    if (got <= 0) return false;

    out[got] = '\0';
    out[strcspn(out, "\n")] = '\0';
    return true;
}

static bool find_xinput(char *node, size_t capacity)
{
    DIR *directory = opendir("/sys/bus/usb/devices");
    if (directory == nullptr) return false;

    const struct dirent *entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (entry->d_name[0] == '.') continue;

        char vendor[16];
        char product[16];
        if (!read_attr(entry->d_name, "idVendor", vendor, sizeof vendor)) continue;
        if (!read_attr(entry->d_name, "idProduct", product, sizeof product)) continue;
        if (strcmp(vendor, BD_VID) != 0 || strcmp(product, BD_PID_XINPUT) != 0) continue;

        char bus[8];
        char number[8];
        if (!read_attr(entry->d_name, "busnum", bus, sizeof bus)) continue;
        if (!read_attr(entry->d_name, "devnum", number, sizeof number)) continue;

        const int written = snprintf(node, capacity, "/dev/bus/usb/%03d/%03d",
                                     atoi(bus), atoi(number));
        closedir(directory);
        return written > 0 && (size_t)written < capacity;
    }
    closedir(directory);
    return false;
}

bool bd_usb_has_xinput(void)
{
    char node[64];
    return find_xinput(node, sizeof node);
}

bool bd_usb_switch_mode(uint8_t mode)
{
    char node[64];
    if (!find_xinput(node, sizeof node)) return false;

    const int fd = open(node, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "nao abri %s: %s\n", node, strerror(errno));
        return false;
    }

    /* USBDEVFS_DISCONNECT nao derruba driver em kernel atual: o que funciona e
     * DISCONNECT_CLAIM (desconecta e reivindica num ioctl so). */
    struct usbdevfs_disconnect_claim claim = {.interface = 0, .flags = 0};
    if (ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &claim) < 0) {
        fprintf(stderr, "disconnect+claim da interface 0 falhou: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    uint8_t frame[BD_MODE_FRAME_LEN] = {0x81, 0x05, 0x00, 0x51, mode};

    /* SUBMITURB exige o campo preenchido; o controle re-enumera assim que
     * processa o frame, entao o reap pode falhar com ENODEV — isso e sucesso. */
    struct usbdevfs_urb urb = {
        .type = USBDEVFS_URB_TYPE_INTERRUPT,
        .endpoint = BD_VENDOR_ENDPOINT,
        .buffer = frame,
        .buffer_length = sizeof frame,
    };
    const bool submitted = ioctl(fd, USBDEVFS_SUBMITURB, &urb) == 0;
    if (submitted) {
        struct usbdevfs_urb *completed = nullptr;
        (void)ioctl(fd, USBDEVFS_REAPURB, &completed);
    }

    (void)ioctl(fd, USBDEVFS_RELEASEINTERFACE, &claim.interface);
    close(fd);
    return submitted;
}
