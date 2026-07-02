/*
 * pps_edge_log — log /dev/pps0 assert edges as raw CSV (RFC 2783 PPS API).
 *
 * The DIRECT PPS path for the Tier 1 timing test (doc/pps_timing.md): the GPSDO
 * 1PPS wired to a Pi GPIO and surfaced by the pps-gpio kernel module at
 * /dev/pps0, kernel-timestamped in the (chrony/GPSDO-disciplined) system clock.
 * Each rising ("assert") edge is one row: the kernel sequence counter and the
 * capture timestamp. Offline, these edges are joined by number against the
 * EXTRACTED in-band marker (pps_integrity) to form the Tier 1 residual — both
 * come from the SAME GPSDO pulse, so differencing them cancels the GPSDO's own
 * error and leaves only what the extraction adds.
 *
 * MEASUREMENT ONLY — no verdict. It records edges and their timestamps; the
 * sequence counter localizes any missed/duplicated edge, but whether the timing
 * is "good" is decided offline (like every other tool in this kit).
 *
 * Timestamps are in whatever clock pps-gpio stamps with (CLOCK_REALTIME on a
 * standard build); with chrony disciplining the host to the GPSDO, that second
 * boundary is GPSDO-traceable, so the sub-second part is the edge's phase.
 *
 * Build note: needs <sys/timepps.h> (Debian/RPi: package pps-tools). If that
 * header is absent the tool still builds, but as a stub that explains what to
 * install — so `make all` / `make check` stay green on machines without it.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PROG_NAME "pps_edge_log"

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [seconds] [--device /dev/pps0] [-o FILE]\n"
        "  seconds            Run duration in seconds (default 0 = until ^C)\n"
        "  -d, --device DEV   PPS source device (default /dev/pps0)\n"
        "  -o, --out FILE     Write CSV here (default stdout)\n"
        "  -h, --help         Show this help\n"
        "\n"
        "Logs /dev/pps0 assert edges as raw CSV (edge,seq,assert_sec,assert_nsec)\n"
        "for the Tier 1 timing join — measurement only, no verdict.\n",
        argv0);
}

#if defined(__has_include)
#  if __has_include(<sys/timepps.h>)
#    define HAVE_TIMEPPS 1
#  endif
#endif

#ifndef HAVE_TIMEPPS
/* No RFC 2783 header on this build host: keep the target buildable (so `make
 * all`/`make check` pass everywhere) but tell the operator what to install.
 * --help still works and exits 0 so the smoke test is portable. */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
    fprintf(stderr, "%s: built without <sys/timepps.h> — install pps-tools "
            "(Debian/RPi: apt install pps-tools) and rebuild.\n", PROG_NAME);
    return 3;
}
#else
#include <fcntl.h>
#include <sys/timepps.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

int main(int argc, char **argv)
{
    const char *device = "/dev/pps0";
    const char *out_path = NULL;
    double seconds = 0.0;

    static struct option opts[] = {
        {"device", required_argument, 0, 'd'},
        {"out",    required_argument, 0, 'o'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "d:o:h", opts, NULL)) != -1) {
        switch (c) {
        case 'd': device = optarg; break;
        case 'o': out_path = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind < argc) {
        char *end = NULL;
        seconds = strtod(argv[optind], &end);
        if (end == argv[optind] || *end != '\0' || seconds < 0) {
            fprintf(stderr, "%s: bad seconds '%s'\n", PROG_NAME, argv[optind]);
            return 2;
        }
        optind++;
    }
    if (optind < argc) { usage(argv[0]); return 2; }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int fd = open(device, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "%s: open %s: %s\n", PROG_NAME, device, strerror(errno));
        return 1;
    }

    pps_handle_t pps;
    if (time_pps_create(fd, &pps) < 0) {
        fprintf(stderr, "%s: time_pps_create(%s): %s (not a PPS device?)\n",
                PROG_NAME, device, strerror(errno));
        close(fd);
        return 1;
    }

    /* Require assert-edge capture; keep any mode bits already set. */
    pps_params_t params;
    int mode = 0;
    if (time_pps_getcap(pps, &mode) < 0) {
        fprintf(stderr, "%s: time_pps_getcap: %s\n", PROG_NAME, strerror(errno));
        time_pps_destroy(pps); close(fd); return 1;
    }
    if (!(mode & PPS_CAPTUREASSERT)) {
        fprintf(stderr, "%s: %s cannot capture the assert edge "
                "(capabilities 0x%x)\n", PROG_NAME, device, mode);
        time_pps_destroy(pps); close(fd); return 1;
    }
    if (time_pps_getparams(pps, &params) < 0) {
        fprintf(stderr, "%s: time_pps_getparams: %s\n", PROG_NAME, strerror(errno));
        time_pps_destroy(pps); close(fd); return 1;
    }
    params.mode |= PPS_CAPTUREASSERT | PPS_TSFMT_TSPEC;
    if (time_pps_setparams(pps, &params) < 0) {
        fprintf(stderr, "%s: time_pps_setparams: %s\n", PROG_NAME, strerror(errno));
        time_pps_destroy(pps); close(fd); return 1;
    }

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "%s: open %s: %s\n", PROG_NAME, out_path,
                    strerror(errno));
            time_pps_destroy(pps); close(fd); return 1;
        }
    }

    fprintf(out, "# %s device=%s clock=system(disciplined) "
            "cols: edge,seq,assert_sec,assert_nsec\n", PROG_NAME, device);
    fprintf(out, "edge,seq,assert_sec,assert_nsec\n");
    fflush(out);

    fprintf(stderr, "%s: logging %s assert edges%s%s\n", PROG_NAME, device,
            seconds > 0 ? " for " : " until ^C",
            seconds > 0 ? "" : "");
    if (seconds > 0)
        fprintf(stderr, "%s: (%.0f s)\n", PROG_NAME, seconds);

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint64_t edge = 0;
    unsigned long last_seq = 0;
    int have_last = 0;

    while (!g_stop) {
        if (seconds > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double el = (double)(now.tv_sec - t0.tv_sec)
                      + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
            if (el >= seconds) break;
        }

        pps_info_t info;
        /* Block up to ~2 s for the next edge, then loop so ^C / duration are
         * responsive even if the PPS stops. */
        struct timespec timeout = { .tv_sec = 2, .tv_nsec = 0 };
        int r = time_pps_fetch(pps, PPS_TSFMT_TSPEC, &info, &timeout);
        if (r < 0) {
            if (errno == EINTR || errno == ETIMEDOUT) continue;
            fprintf(stderr, "%s: time_pps_fetch: %s\n", PROG_NAME,
                    strerror(errno));
            break;
        }

        unsigned long seq = info.assert_sequence;
        if (have_last && seq == last_seq) continue;   /* no new edge yet */
        have_last = 1;
        last_seq = seq;

        fprintf(out, "%" PRIu64 ",%lu,%lld,%ld\n", edge, seq,
                (long long)info.assert_timestamp.tv_sec,
                (long)info.assert_timestamp.tv_nsec);
        fflush(out);
        edge++;
    }

    fprintf(out, "# end edges=%" PRIu64 "\n", edge);
    if (out != stdout) fclose(out);
    fprintf(stderr, "%s: %" PRIu64 " edge(s) logged\n", PROG_NAME, edge);

    time_pps_destroy(pps);
    close(fd);
    return 0;
}
#endif /* HAVE_TIMEPPS */
