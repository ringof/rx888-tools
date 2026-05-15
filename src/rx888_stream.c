/*
 * rx888_stream — thin CLI wrapper over librx888.
 *
 * Replaces the previous rx888_stream.c. All the libusb / threading /
 * descriptor logic now lives in librx888; this file just parses
 * options, opens the device, registers a "write samples to stdout"
 * callback, and waits for SIGINT.
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

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options] > iq.raw\n"
        "  -f, --firmware <file>      Upload firmware if device in boot mode\n"
        "  -s, --samplerate <Hz>      Sample rate (32000000 or 135000000)\n"
        "  -d, --dither               Enable dither\n"
        "  -r, --rand                 Enable randomizer (also enables fixup)\n"
        "  -m, --gainmode <low|high>  Gain mode (default high)\n"
        "  -g, --gain <0..127>        VGA gain code\n"
        "  -a, --att <0..63>          DAT-31 attenuator (half-dB steps)\n"
        "  -h, --help                 This help\n",
        argv0);
}

int main(int argc, char **argv) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    rx888_config_t cfg = {
        .samplerate    = 32000000u,
        .att           = 0,
        .gain          = 0,
        .gain_high     = 1,
        .dither        = 0,
        .randomizer    = 0,
        .fixup_samples = 0,
        .firmware_path = NULL,
    };

    static struct option opts[] = {
        {"firmware",   required_argument, 0, 'f'},
        {"samplerate", required_argument, 0, 's'},
        {"dither",     no_argument,       0, 'd'},
        {"rand",       no_argument,       0, 'r'},
        {"gainmode",   required_argument, 0, 'm'},
        {"gain",       required_argument, 0, 'g'},
        {"att",        required_argument, 0, 'a'},
        {"help",       no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "f:s:drm:g:a:h", opts, NULL)) != -1) {
        switch (c) {
        case 'f': cfg.firmware_path = optarg; break;
        case 's': cfg.samplerate = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'd': cfg.dither = 1; break;
        case 'r': cfg.randomizer = 1; cfg.fixup_samples = 1; break;
        case 'm':
            if (!strcmp(optarg, "low"))       cfg.gain_high = 0;
            else if (!strcmp(optarg, "high")) cfg.gain_high = 1;
            else { fprintf(stderr, "%s: bad gainmode\n", PROG_NAME); return 2; }
            break;
        case 'g': cfg.gain = (unsigned)strtoul(optarg, NULL, 10) & 0x7fu; break;
        case 'a': cfg.att  = (unsigned)strtoul(optarg, NULL, 10) & 0x3fu; break;
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

    while (!g_stop) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    rx888_close(r);
    return 0;
}
