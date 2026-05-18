/*
 * rx888_stream — thin CLI wrapper over librx888.
 *
 * All the libusb / threading / descriptor logic lives in librx888;
 * this file parses options, opens the device, registers a "write
 * samples to stdout" callback, and waits for SIGINT.  -v prints
 * librx888 stats every second to stderr.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "librx888.h"

#define PROG_NAME "rx888_stream"

static volatile sig_atomic_t g_stop = 0;
static atomic_bool g_pipe_broken = false;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) { errno = EIO; return -1; }
        p += (size_t)n; len -= (size_t)n;
    }
    return 0;
}

static void sample_cb(const int16_t *samples, size_t nsamples, void *user) {
    (void)user;
    if (atomic_load(&g_pipe_broken) || g_stop) return;
    if (write_all(STDOUT_FILENO, samples, nsamples * sizeof(int16_t)) != 0) {
        atomic_store(&g_pipe_broken, true);
        g_stop = 1;
    }
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options] > iq.raw\n"
        "  -f, --firmware <file>       Upload firmware if device is in boot mode\n"
        "  -v, --verbose               Print stats once per second to stderr\n"
        "  -d, --dither                Enable dither GPIO\n"
        "  -r, --rand                  Enable randomizer GPIO + sample fixup\n"
        "      --fixup                 Enable sample fixup independently of -r\n"
        "  -s, --samplerate <Hz>       Sample rate (e.g. 32000000, 135000000)\n"
        "  -m, --gainmode <low|high>   Gain mode (default high)\n"
        "  -g, --gain <0..127>         VGA gain code\n"
        "  -a, --att <0..63>           DAT-31 attenuator (half-dB steps)\n"
        "  -q, --queuedepth <N>        Concurrent USB transfers (default 32)\n"
        "  -p, --reqsize <N>           Transfer size in packets (default 1024)\n"
        "      --ctrl-timeout <ms>     Control transfer timeout (default 5000)\n"
        "      --stream-timeout <ms>   Bulk transfer timeout (default 0 = infinite)\n"
        "      --watchdog-timeout <ms> No-data watchdog (default 3000; 0 disables)\n"
        "  -h, --help                  Show this help\n",
        argv0);
}

enum {
    OPT_FIXUP = 0x100,
    OPT_CTRL_TIMEOUT,
    OPT_STREAM_TIMEOUT,
    OPT_WATCHDOG_TIMEOUT,
};

int main(int argc, char **argv) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    rx888_config_t cfg;
    rx888_config_init_default(&cfg);
    int verbose = 0;

    static struct option opts[] = {
        {"firmware",         required_argument, 0, 'f'},
        {"verbose",          no_argument,       0, 'v'},
        {"samplerate",       required_argument, 0, 's'},
        {"dither",           no_argument,       0, 'd'},
        {"rand",             no_argument,       0, 'r'},
        {"fixup",            no_argument,       0, OPT_FIXUP},
        {"gainmode",         required_argument, 0, 'm'},
        {"gain",             required_argument, 0, 'g'},
        {"att",              required_argument, 0, 'a'},
        {"queuedepth",       required_argument, 0, 'q'},
        {"reqsize",          required_argument, 0, 'p'},
        {"ctrl-timeout",     required_argument, 0, OPT_CTRL_TIMEOUT},
        {"stream-timeout",   required_argument, 0, OPT_STREAM_TIMEOUT},
        {"watchdog-timeout", required_argument, 0, OPT_WATCHDOG_TIMEOUT},
        {"help",             no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "f:vs:drm:g:a:q:p:h", opts, NULL)) != -1) {
        switch (c) {
        case 'f': cfg.firmware_path = optarg; break;
        case 'v': verbose = 1; break;
        case 's': cfg.samplerate = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'd': cfg.dither = 1; break;
        case 'r': cfg.randomizer = 1; cfg.fixup_samples = 1; break;
        case OPT_FIXUP: cfg.fixup_samples = 1; break;
        case 'm':
            if (!strcmp(optarg, "low"))       cfg.gain_high = 0;
            else if (!strcmp(optarg, "high")) cfg.gain_high = 1;
            else { fprintf(stderr, "%s: bad gainmode\n", PROG_NAME); return 2; }
            break;
        case 'g': cfg.gain = (unsigned)strtoul(optarg, NULL, 10) & 0x7fu; break;
        case 'a': cfg.att  = (unsigned)strtoul(optarg, NULL, 10) & 0x3fu; break;
        case 'q': cfg.queue_depth = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'p': cfg.req_packets = (unsigned)strtoul(optarg, NULL, 10); break;
        case OPT_CTRL_TIMEOUT:     cfg.ctrl_timeout_ms   = (unsigned)strtoul(optarg, NULL, 10); break;
        case OPT_STREAM_TIMEOUT:   cfg.stream_timeout_ms = (unsigned)strtoul(optarg, NULL, 10); break;
        case OPT_WATCHDOG_TIMEOUT: cfg.watchdog_ms       = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    if (isatty(STDOUT_FILENO)) {
        fprintf(stderr, "%s: stdout is a TTY; redirect to a file or pipe\n", PROG_NAME);
        return 2;
    }

    rx888_t *r = NULL;
    int rc = rx888_open(&r, &cfg);
    if (rc != 0) {
        fprintf(stderr, "%s: rx888_open: %s\n", PROG_NAME, rx888_strerror(rc));
        return 1;
    }

    rc = rx888_start(r, sample_cb, NULL);
    if (rc != 0) {
        fprintf(stderr, "%s: rx888_start: %s\n", PROG_NAME, rx888_strerror(rc));
        rx888_close(r);
        return 1;
    }

    uint64_t t0 = monotonic_ms();
    uint64_t prev_bytes = 0;
    int internal_stop = 0;
    while (!g_stop) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        /* Notice if librx888 stopped internally (NO_DEVICE, watchdog
         * fire, libusb error) and exit accordingly — otherwise the
         * CLI would sleep forever after a hardware failure. */
        if (!rx888_is_running(r)) {
            internal_stop = 1;
            break;
        }

        if (verbose) {
            static uint64_t next_print = 0;
            uint64_t now = monotonic_ms();
            if (next_print == 0) next_print = now + 1000;
            if (now >= next_print) {
                rx888_stats_t s;
                rx888_get_stats(r, &s);
                double secs = (now - t0) / 1000.0;
                double mibs = (double)(s.bytes_out - prev_bytes) / (1024.0 * 1024.0);
                fprintf(stderr,
                    "t=%.0fs ok=%llu bad=%llu in_flight=%u "
                    "out=%.2f MiB (%.2f MiB/s) "
                    "full=%llu short=%llu last_window=%llu\n",
                    secs, s.ok_xfers, s.bad_xfers, s.in_flight,
                    (double)s.bytes_out / (1024.0 * 1024.0), mibs,
                    s.full_xfers, s.short_xfers, s.last_window_bytes);
                prev_bytes = s.bytes_out;
                next_print += 1000;
            }
        }
    }

    rx888_close(r);
    if (internal_stop) {
        fprintf(stderr, "%s: stream stopped by library "
                "(device error, watchdog, or libusb failure)\n", PROG_NAME);
        return 1;
    }
    return 0;
}
