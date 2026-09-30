// ls_test_input.c - inject uinput events to navigate TrimUI menu
// Compile: zig cc -target aarch64-linux-gnu -static -O2 ls_test_input.c -o /tmp/ls_test_input
// Run:     /tmp/ls_test_input ls_home | a | b | right N | left N | down N
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>

// zig libc thiếu các macro KEY_DPAD_*
#ifndef KEY_DPAD_UP
#define KEY_DPAD_UP    0x13a
#endif
#ifndef KEY_DPAD_DOWN
#define KEY_DPAD_DOWN  0x13b
#endif
#ifndef KEY_DPAD_LEFT
#define KEY_DPAD_LEFT  0x13c
#endif
#ifndef KEY_DPAD_RIGHT
#define KEY_DPAD_RIGHT 0x13d
#endif
#ifndef KEY_START
#define KEY_START 0x13e
#endif
#ifndef KEY_SELECT
#define KEY_SELECT 0x13f
#endif
#ifndef KEY_MENU
#define KEY_MENU 0x137
#endif

static int fd = -1;

static void emit(int type, int code, int val) {
    struct input_event ev = { .type = type, .code = code, .value = val };
    write(fd, &ev, sizeof(ev));
}

static void press(int key, int hold_ms) {
    emit(EV_KEY, key, 1);
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(hold_ms * 1000);
    emit(EV_KEY, key, 0);
    emit(EV_SYN, SYN_REPORT, 0);
}

static int setup(void) {
    fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open uinput"); return 0; }

    // Enable event types
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);

    // Enable keys we need
    ioctl(fd, UI_SET_KEYBIT, KEY_DPAD_UP);
    ioctl(fd, UI_SET_KEYBIT, KEY_DPAD_DOWN);
    ioctl(fd, UI_SET_KEYBIT, KEY_DPAD_LEFT);
    ioctl(fd, UI_SET_KEYBIT, KEY_DPAD_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, KEY_A);
    ioctl(fd, UI_SET_KEYBIT, KEY_B);
    ioctl(fd, UI_SET_KEYBIT, KEY_X);
    ioctl(fd, UI_SET_KEYBIT, KEY_Y);
    ioctl(fd, UI_SET_KEYBIT, KEY_START);
    ioctl(fd, UI_SET_KEYBIT, KEY_SELECT);
    ioctl(fd, UI_SET_KEYBIT, KEY_MENU);

    // Setup device
    struct uinput_setup dev = {0};
    strcpy(dev.name, "RomCloudTestPad");
    dev.id.bustype = BUS_USB;
    dev.id.vendor  = 0x1234;
    dev.id.product = 0x5678;
    dev.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &dev) < 0) { perror("UI_DEV_SETUP"); return 0; }

    if (ioctl(fd, UI_DEV_CREATE, 0) < 0) { perror("UI_DEV_CREATE"); return 0; }
    return 1;
}

static void teardown(void) {
    ioctl(fd, UI_DEV_DESTROY, 0);
    close(fd);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s ls_home | a | b | right N | left N | down N | up N\n", argv[0]);
        return 2;
    }
    if (!setup()) return 1;
    usleep(300 * 1000);

    const char* cmd = argv[1];
    int n = (argc >= 3) ? atoi(argv[2]) : 1;

    if (!strcmp(cmd, "ls_home")) {
        // From MENU index 0 (games), go RIGHT 4 to index 4 (localsend), then A
        for (int i = 0; i < 4; i++) press(KEY_DPAD_RIGHT, 100);
        usleep(200 * 1000);
        press(KEY_A, 100);
    } else if (!strcmp(cmd, "a")) {
        press(KEY_A, 100);
    } else if (!strcmp(cmd, "b")) {
        press(KEY_B, 100);
    } else if (!strcmp(cmd, "right")) {
        for (int i = 0; i < n; i++) press(KEY_DPAD_RIGHT, 100);
    } else if (!strcmp(cmd, "left")) {
        for (int i = 0; i < n; i++) press(KEY_DPAD_LEFT, 100);
    } else if (!strcmp(cmd, "down")) {
        for (int i = 0; i < n; i++) press(KEY_DPAD_DOWN, 100);
    } else if (!strcmp(cmd, "up")) {
        for (int i = 0; i < n; i++) press(KEY_DPAD_UP, 100);
    } else {
        fprintf(stderr, "Unknown cmd: %s\n", cmd);
        teardown();
        return 2;
    }

    usleep(200 * 1000);
    teardown();
    return 0;
}
