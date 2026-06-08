/*
 * fx3_cmd.c — RX888mk2 vendor-command diagnostics CLI.
 *
 * A bench/operator diagnostics tool for the SDDC_FX3 firmware: probe the
 * device, poke individual vendor commands (GPIO, ADC clock, attenuator, VGA,
 * I2C, GPIF start/stop), read GETSTATS counters, recover a wedged device
 * (reset / usbreset / reload), and drop into an interactive debug console.
 *
 * Built on the shared diagnostics core (fx3_core / fx3_usb / fx3_stats),
 * extracted from the rx888-firmware test harness.  The firmware regression/
 * fuzz/soak scenarios stay in that repo; this binary ships only the operator
 * diagnostics.
 *
 * Build:  make fx3_cmd        (needs libusb-1.0-0-dev)
 * Needs:  rx888_stream on PATH or alongside this binary for load/reload.
 *
 * Copyright (c) 2024-2026 David Goncalves — MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <fcntl.h>
#include <libusb-1.0/libusb.h>

#include "rx888.h"
#include "fx3_usb.h"
#include "fx3_stats.h"
#include "fx3_core.h"

/* ------------------------------------------------------------------ */
/* Interactive debug console ('!' escape for local commands)          */
/* ------------------------------------------------------------------ */

struct local_cmd_entry {
    const char *name;
    int (*func)(libusb_device_handle *);
};

/* No-arg local commands available inside the debug console.  Only the
 * operator-diagnostics subset is exposed here (the firmware harness scenarios
 * live in rx888-firmware, not in this binary). */
static const struct local_cmd_entry local_cmds_noarg[] = {
    {"test",       do_test},
    {"start",      do_start},
    {"stop",       do_stop},
    {"stats",      do_stats},
    {"stack_check", do_test_stack_check},
    {"stats_pll",  do_test_stats_pll},
    {"stats_shdn", do_test_stats_shdn},
    {"reset",      do_reset},
    {NULL, NULL}
};

static void print_local_help(void)
{
    printf("Local commands (prefix with '!'):\n"
           "  help / ?                      This help\n"
           "  test                          Read device info\n"
           "  start / stop                  Start/stop GPIF streaming\n"
           "  adc <freq>                    Set ADC clock frequency\n"
           "  att <0-63>                    Set DAT-31 attenuator\n"
           "  vga <0-255>                   Set AD8370 VGA gain\n"
           "  wdg_max <0-255>              Set watchdog max recovery count (0=unlimited)\n"
           "  gpio <bits>                   Set GPIO word\n"
           "  stats                         Read GETSTATS counters\n"
           "  stats_pll                     Verify Si5351 PLL lock\n"
           "  stats_shdn                    SHDN asserted after STOPFX3 (#131)\n"
           "  stack_check                   Query firmware stack watermark\n"
           "  i2cr <addr> <reg> <len>       I2C read (hex)\n"
           "  i2cw <addr> <reg> <byte>...   I2C write (hex)\n"
           "  raw <code>                    Send raw vendor request (hex)\n"
           "  reset                         Reboot FX3 to bootloader\n");
}

/* Parse and dispatch a local command line (without the '!' prefix). */
static int dispatch_local_cmd(libusb_device_handle *h, const char *line)
{
    while (*line == ' ') line++;
    if (*line == '\0') return 0;

    char cmd[64] = {0};
    const char *args = NULL;
    const char *sp = strchr(line, ' ');
    if (sp) {
        int len = (int)(sp - line);
        if (len >= (int)sizeof(cmd)) len = (int)sizeof(cmd) - 1;
        memcpy(cmd, line, len);
        args = sp + 1;
        while (*args == ' ') args++;
        if (*args == '\0') args = NULL;
    } else {
        int len = (int)strlen(line);
        if (len >= (int)sizeof(cmd)) len = (int)sizeof(cmd) - 1;
        memcpy(cmd, line, len);
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_local_help();
        return 0;
    }

    for (const struct local_cmd_entry *e = local_cmds_noarg; e->name; e++) {
        if (strcmp(cmd, e->name) == 0)
            return e->func(h);
    }

    if (strcmp(cmd, "adc") == 0) {
        if (!args) { printf("usage: adc <freq_hz>\n"); return 1; }
        return do_adc(h, (uint32_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "att") == 0) {
        if (!args) { printf("usage: att <0-63>\n"); return 1; }
        return do_att(h, (uint16_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "vga") == 0) {
        if (!args) { printf("usage: vga <0-255>\n"); return 1; }
        return do_vga(h, (uint16_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "wdg_max") == 0) {
        if (!args) { printf("usage: wdg_max <0-255>\n"); return 1; }
        return do_wdg_max(h, (uint16_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "gpio") == 0) {
        if (!args) { printf("usage: gpio <bits>\n"); return 1; }
        return do_gpio(h, (uint32_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "raw") == 0) {
        if (!args) { printf("usage: raw <code>\n"); return 1; }
        return do_raw(h, (uint8_t)strtoul(args, NULL, 0));
    }
    if (strcmp(cmd, "i2cr") == 0) {
        if (!args) { printf("usage: i2cr <addr> <reg> <len>\n"); return 1; }
        unsigned long a, rg, l;
        if (sscanf(args, "%li %li %li", &a, &rg, &l) != 3) {
            printf("usage: i2cr <addr> <reg> <len>\n");
            return 1;
        }
        return do_i2cr(h, (uint16_t)a, (uint16_t)rg, (uint16_t)l);
    }
    if (strcmp(cmd, "i2cw") == 0) {
        if (!args) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        char *p = (char *)args;
        char *end;
        unsigned long a = strtoul(p, &end, 0);
        if (end == p) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        p = end;
        unsigned long rg = strtoul(p, &end, 0);
        if (end == p) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        p = end;
        uint8_t data[64];
        int ndata = 0;
        while (ndata < (int)sizeof(data)) {
            unsigned long b = strtoul(p, &end, 0);
            if (end == p) break;
            data[ndata++] = (uint8_t)b;
            p = end;
        }
        if (ndata == 0) { printf("usage: i2cw <addr> <reg> <byte>...\n"); return 1; }
        return do_i2cw(h, (uint16_t)a, (uint16_t)rg, data, (uint16_t)ndata);
    }

    printf("unknown local command: '%s' (type !help for list)\n", cmd);
    return 1;
}

/* SIGINT handler: restore terminal from raw mode before exit. */
static struct termios saved_termios;
static volatile sig_atomic_t raw_mode_active;

static void sigint_handler(int sig)
{
    if (raw_mode_active)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Interactive debug console over USB.  Sends TESTFX3 wValue=1 to enable
 * debug mode, then polls READINFODEBUG for output.  Typed characters are
 * forwarded in wValue; CR triggers command execution on the FX3 side.
 * '!' switches to local command mode (see dispatch_local_cmd).  Ctrl-C exits. */
static int do_debug(libusb_device_handle *h)
{
    uint8_t info[4] = {0};
    int r = ctrl_read(h, TESTFX3, 1, 0, info, 4);
    if (r < 0) {
        printf("FAIL debug: enable debug mode: %s\n", libusb_strerror(r));
        return 1;
    }
    printf("debug: enabled (hwconfig=0x%02X fw=%d.%d)\n",
           info[0], info[1], info[2]);
    printf("debug: type commands + Enter for FX3, '!' for local commands, Ctrl-C to quit\n");
    fflush(stdout);

    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    saved_termios = oldt;
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    raw_mode_active = 1;
    signal(SIGINT, sigint_handler);

    int local_mode = 0;
    char local_buf[128];
    int local_len = 0;

    uint8_t buf[64];
    for (;;) {
        uint16_t send_char = 0;
        char ch;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
            if (!local_mode && ch == '!') {
                local_mode = 1;
                local_len = 0;
                printf("\nfx3> ");
                fflush(stdout);
            } else if (local_mode) {
                if (ch == '\n' || ch == '\r') {
                    local_buf[local_len] = '\0';
                    printf("\n");
                    fflush(stdout);
                    if (local_len > 0)
                        dispatch_local_cmd(h, local_buf);
                    fflush(stdout);
                    local_mode = 0;
                } else if (ch == 0x7f || ch == 0x08) {
                    if (local_len > 0) {
                        local_len--;
                        printf("\b \b");
                        fflush(stdout);
                    }
                } else if (ch == 0x03 || ch == 0x1b) {
                    printf(" (cancelled)\n");
                    fflush(stdout);
                    local_mode = 0;
                } else if (local_len < (int)sizeof(local_buf) - 1) {
                    local_buf[local_len++] = ch;
                    putchar(ch);
                    fflush(stdout);
                }
            } else {
                if (ch == '\n') ch = '\r';
                send_char = (uint8_t)ch;
            }
        }

        r = ctrl_read(h, READINFODEBUG, send_char, 0, buf, sizeof(buf));
        if (r > 0) {
            buf[r - 1] = '\0';
            printf("%s", (char *)buf);
            fflush(stdout);
        }

        usleep(50000);
    }
    /* NOTREACHED — loop exits via SIGINT → sigint_handler */
}

/* ------------------------------------------------------------------ */
/* Argument parsing + dispatch                                        */
/* ------------------------------------------------------------------ */

static unsigned long parse_num(const char *s)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 0);  /* 0 = auto-detect base */
    if (errno || *end != '\0') {
        fprintf(stderr, "error: invalid number '%s'\n", s);
        exit(2);
    }
    return v;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-F firmware.img] <command> [args...]\n"
        "\n"
        "Options:\n"
        "  -F, --firmware <path>        Upload firmware first if device is in\n"
        "                               bootloader mode (PID 0x00F3)\n"
        "\n"
        "Commands:\n"
        "  load <firmware.img>          Upload firmware and exit\n"
        "  reload [firmware.img]        Reset to bootloader (RESETFX3 + usbreset),\n"
        "                               re-upload firmware, verify healthy\n"
        "  test                         Read device info (TESTFX3)\n"
        "  gpio <bits>                  Set GPIO word (hex or decimal)\n"
        "  adc <freq_hz>               Set ADC clock frequency (STARTADC)\n"
        "  att <0-63>                   Set DAT-31 attenuator\n"
        "  vga <0-255>                  Set AD8370 VGA gain\n"
        "  wdg_max <0-255>             Set watchdog max recovery count (0=unlimited)\n"
        "  start                        Start streaming (STARTFX3)\n"
        "  stop                         Stop streaming (STOPFX3)\n"
        "  i2cr <addr> <reg> <len>      I2C read (hex addresses)\n"
        "  i2cw <addr> <reg> <byte>...  I2C write (hex addresses, hex data)\n"
        "  reset                        Reboot FX3 to bootloader (RESETFX3)\n"
        "  usbreset                     Host-side USB port reset (USBDEVFS_RESET)\n"
        "  debug                        Interactive debug console over USB\n"
        "  raw <code>                   Send raw vendor request (hex)\n"
        "  stats                        Read GETSTATS diagnostic counters\n"
        "  stats_pll                    Verify Si5351 PLL lock status\n"
        "  stats_shdn                   SHDN asserted after STOPFX3 (#131)\n"
        "  stack_check                  Query firmware stack watermark\n"
        "\n"
        "Output:  PASS/FAIL <command> [details]\n"
        "Exit:    0 on PASS, 1 on FAIL\n",
        prog);
}

int main(int argc, char **argv)
{
    /* ---- Parse -F / --firmware option ---- */
    const char *firmware_path = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-F") == 0 ||
            strcmp(argv[i], "--firmware") == 0) {
            firmware_path = argv[i + 1];
            int remaining = argc - i - 2;
            memmove(&argv[i], &argv[i + 2], remaining * sizeof(char *));
            argc -= 2;
            argv[argc] = NULL;
            break;
        }
    }

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    const char *cmd = argv[1];

    /* Help is answered before touching libusb so it works with no device
     * attached (and without needing usb permissions). */
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "help") == 0) {
        usage(argv[0]);
        return 0;
    }

    libusb_context *ctx = NULL;
    int r = libusb_init(&ctx);
    if (r < 0) {
        fprintf(stderr, "error: libusb_init: %s\n", libusb_strerror(r));
        return 1;
    }
    g_ctx = ctx;
    g_firmware_path = firmware_path;

    /* ---- "load" (upload-only; no app-mode device needed) ---- */
    if (strcmp(cmd, "load") == 0) {
        const char *fw = (argc >= 3) ? argv[2] : firmware_path;
        if (!fw) {
            fprintf(stderr, "error: load requires a firmware path\n"
                            "usage: %s load <firmware.img>\n", argv[0]);
            libusb_exit(ctx);
            return 2;
        }
        libusb_device_handle *app =
            libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_APP);
        if (app) {
            fprintf(stderr, "device in app mode — resetting to bootloader...\n");
            if (libusb_kernel_driver_active(app, 0) == 1)
                libusb_detach_kernel_driver(app, 0);
            libusb_claim_interface(app, 0);
            cmd_u32(app, RESETFX3, 0);
            libusb_close(app);
            sleep(3);
        }
        int rc = upload_firmware(ctx, fw);
        libusb_exit(ctx);
        return (rc == 0) ? 0 : 1;
    }

    /* ---- "usbreset" (raw USBDEVFS_RESET, no claim) ----
     * Handled before open so it can recover a wedged device. */
    if (strcmp(cmd, "usbreset") == 0) {
        int rc = do_usbreset(ctx);
        libusb_exit(ctx);
        return rc;
    }

    /* ---- "reload" (reset -> re-upload -> verify) ---- */
    if (strcmp(cmd, "reload") == 0) {
        const char *fw = (argc >= 3) ? argv[2] : firmware_path;
        if (!fw) {
            fprintf(stderr, "error: reload requires a firmware path\n"
                            "usage: %s [-F img] reload [firmware.img]\n", argv[0]);
            libusb_exit(ctx);
            return 2;
        }
        int rc = do_reload(ctx, fw);
        libusb_exit(ctx);
        return rc;
    }

    /* ---- Auto-upload if -F given and device is in bootloader mode ---- */
    if (firmware_path) {
        libusb_device_handle *boot =
            libusb_open_device_with_vid_pid(ctx, RX888_VID, RX888_PID_BOOT);
        if (boot) {
            libusb_close(boot);
            fprintf(stderr, "device in bootloader mode "
                    "— uploading firmware...\n");
            if (upload_firmware(ctx, firmware_path) != 0) {
                libusb_exit(ctx);
                return 1;
            }
        }
    }

    libusb_device_handle *h = open_rx888(ctx);
    if (!h) {
        libusb_exit(ctx);
        return 1;
    }

    int rc = 1;

    if (strcmp(cmd, "test") == 0) {
        rc = do_test(h);
    } else if (strcmp(cmd, "gpio") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_gpio(h, (uint32_t)parse_num(argv[2]));
    } else if (strcmp(cmd, "adc") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_adc(h, (uint32_t)parse_num(argv[2]));
    } else if (strcmp(cmd, "att") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_att(h, (uint16_t)parse_num(argv[2]));
    } else if (strcmp(cmd, "vga") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_vga(h, (uint16_t)parse_num(argv[2]));
    } else if (strcmp(cmd, "wdg_max") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_wdg_max(h, (uint16_t)parse_num(argv[2]));
    } else if (strcmp(cmd, "start") == 0) {
        rc = do_start(h);
    } else if (strcmp(cmd, "stop") == 0) {
        rc = do_stop(h);
    } else if (strcmp(cmd, "i2cr") == 0) {
        if (argc < 5) { usage(argv[0]); goto out; }
        rc = do_i2cr(h, (uint16_t)parse_num(argv[2]),
                        (uint16_t)parse_num(argv[3]),
                        (uint16_t)parse_num(argv[4]));
    } else if (strcmp(cmd, "i2cw") == 0) {
        if (argc < 5) { usage(argv[0]); goto out; }
        uint16_t addr = (uint16_t)parse_num(argv[2]);
        uint16_t reg  = (uint16_t)parse_num(argv[3]);
        int ndata = argc - 4;
        uint8_t data[64];
        if (ndata > (int)sizeof(data)) ndata = (int)sizeof(data);
        for (int i = 0; i < ndata; i++)
            data[i] = (uint8_t)parse_num(argv[4 + i]);
        rc = do_i2cw(h, addr, reg, data, (uint16_t)ndata);
    } else if (strcmp(cmd, "debug") == 0) {
        rc = do_debug(h);
    } else if (strcmp(cmd, "reset") == 0) {
        rc = do_reset(h);
    } else if (strcmp(cmd, "raw") == 0) {
        if (argc < 3) { usage(argv[0]); goto out; }
        rc = do_raw(h, (uint8_t)parse_num(argv[2]));
    } else if (strcmp(cmd, "stats") == 0) {
        rc = do_stats(h);
    } else if (strcmp(cmd, "stats_pll") == 0) {
        rc = do_test_stats_pll(h);
    } else if (strcmp(cmd, "stats_shdn") == 0) {
        rc = do_test_stats_shdn(h);
    } else if (strcmp(cmd, "stack_check") == 0) {
        rc = do_test_stack_check(h);
    } else {
        fprintf(stderr, "error: unknown command '%s'\n", cmd);
        usage(argv[0]);
        rc = 2;
    }

out:
    close_rx888(h);
    libusb_exit(ctx);
    return rc;
}
