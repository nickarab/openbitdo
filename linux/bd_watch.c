/*
 * bd_watch — mostra ao vivo os botões do 8BitDo Ultimate em DInput (hidraw).
 *
 * Em DInput o mesmo hidraw entrega o report vendor (ID 2) junto com o do
 * gamepad (ID 3); os IDs 2 são descartados. No ID 3, o byte seguinte ao ID
 * traz bits 0..3 = hat e bits 4..7 = A/B/X/Y.
 *
 * _POSIX_C_SOURCE: -std=c23 esconde poll(), open() e O_CLOEXEC.
 */
#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

constexpr uint8_t REPORT_ID_GAMEPAD = 3;
constexpr size_t REPORT_CAPACITY = 64;
constexpr size_t LINE_CAPACITY = 128;
constexpr int POLL_TIMEOUT_MS = 200;

static const char *const BUTTON_NAMES[4] = {"A", "B", "X", "Y"};
static volatile sig_atomic_t stopped;

static void on_signal(int signum) { (void)signum; stopped = 1; }

static void format_pressed(unsigned pressed, char *line, size_t capacity)
{
    size_t used = 0;
    for (size_t i = 0; i < 4 && used < capacity; i++)
        if ((pressed & (1u << i)) != 0)
            used += (size_t)snprintf(line + used, capacity - used, "%s ", BUTTON_NAMES[i]);
    if (used == 0) (void)snprintf(line, capacity, "(nada)");
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/hidraw0";
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    signal(SIGINT, on_signal);
    printf("lendo %s — aperte os botões (Ctrl+C para sair)\n", path);
    fflush(stdout);

    uint8_t report[REPORT_CAPACITY];
    unsigned previous = ~0u; /* nunca ocorre: força imprimir o estado inicial */

    while (!stopped) {
        struct pollfd watcher = {.fd = fd, .events = POLLIN};
        if (poll(&watcher, 1, POLL_TIMEOUT_MS) <= 0) continue;

        ssize_t got = read(fd, report, sizeof report);
        if (got < 2 || report[0] != REPORT_ID_GAMEPAD) continue;

        unsigned pressed = (unsigned)((report[1] >> 4) & 0x0fu);
        if (pressed == previous) continue;
        previous = pressed;

        char line[LINE_CAPACITY];
        format_pressed(pressed, line, sizeof line);
        printf("%-8s |", line);
        for (ssize_t i = 1; i < got; i++) printf(" %02x", (unsigned)report[i]);
        printf("\n");
        fflush(stdout);
    }
    close(fd);
    return 0;
}
