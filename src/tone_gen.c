/*
 * tone_gen — synthetic coherent-tone source for testing the tone_monitor
 * Goertzel stream processor and the tone_quality.py analyzer with no hardware.
 *
 * Emits raw interleaved int16 LE REAL samples of a tone at f_tone, sampled at
 * fs, to stdout. Pipe it into the real DSP path:
 *
 *     tone_gen 2 --drop-every 64800000 | tone_monitor --source - --iqlog t.iq
 *     tone_quality.py t.iq
 *
 * The tone is generated exactly as the rig's coherent tone (default
 * f_tone/fs = 27/129.6 = 5/24), so a clean stream demodulates to a flat phasor.
 * Defects are injected deterministically so a test can assert what the analyzer
 * must find:
 *   --drop-every N    drop one sample every N samples  (a grid slip: +75 deg)
 *   --dup-every N     duplicate one sample every N samples (a slip the other way)
 *   --garble-every N  corrupt a run every N samples (amplitude transient)
 *   --garble-len L    length of each garble run (default 50)
 *   --freq-offset PPM detune the tone (the analyzer should report this as the
 *                     residual carrier — tests the "benign wander" tracking)
 *   --noise LSB       add uniform +/-LSB noise (sets a floor / SNR)
 *
 * Length: positional SECONDS (default 1; 0 = endless until the reader closes),
 * or --samples N for an exact count.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROG_NAME "tone_gen"
#define BLOCK     65536u

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint32_t xorshift(uint32_t *st)
{
    uint32_t x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (*st = x);
}

static int16_t clamp16(double v)
{
    if (v >  32767.0) return  32767;
    if (v < -32768.0) return -32768;
    return (int16_t)lrint(v);
}

static void usage(const char *a0)
{
    fprintf(stderr,
        "Usage: %s [seconds] [--rate MSPS] [--ftone HZ] [--amp LSB]\n"
        "          [--samples N] [--drop-every N] [--dup-every N]\n"
        "          [--garble-every N] [--garble-len L] [--freq-offset PPM]\n"
        "          [--noise LSB]\n"
        "  seconds         Output length in seconds (default 1; 0 = endless)\n"
        "  --rate MSPS     Sample rate (default 129.6)\n"
        "  --ftone HZ      Tone frequency (default 27e6; coherent at 5/24)\n"
        "  --amp LSB       Tone amplitude (default 10000)\n"
        "  --samples N     Exact sample count (overrides seconds)\n"
        "  --drop-every N  Drop one sample every N (inject grid slips)\n"
        "  --dup-every N   Duplicate one sample every N\n"
        "  --garble-every N  Corrupt a run every N samples\n"
        "  --garble-len L  Garble run length (default 50)\n"
        "  --freq-offset PPM  Detune the tone (-> residual carrier)\n"
        "  --noise LSB     Add uniform +/-LSB noise\n"
        "\n"
        "Raw int16 LE REAL samples to stdout. Pipe into tone_monitor --source -.\n",
        a0);
}

int main(int argc, char **argv)
{
    double seconds = 1.0, rate_msps = 129.6, ftone = 27e6, amp = 10000.0;
    double freq_ppm = 0.0, noise = 0.0;
    uint64_t samples = 0, drop_every = 0, dup_every = 0, garble_every = 0;
    unsigned garble_len = 50;

    enum { O_RATE=256,O_FTONE,O_AMP,O_SAMP,O_DROP,O_DUP,O_GARB,O_GLEN,O_FOFF,O_NOISE };
    static struct option opts[] = {
        {"rate",required_argument,0,O_RATE},{"ftone",required_argument,0,O_FTONE},
        {"amp",required_argument,0,O_AMP},{"samples",required_argument,0,O_SAMP},
        {"drop-every",required_argument,0,O_DROP},{"dup-every",required_argument,0,O_DUP},
        {"garble-every",required_argument,0,O_GARB},{"garble-len",required_argument,0,O_GLEN},
        {"freq-offset",required_argument,0,O_FOFF},{"noise",required_argument,0,O_NOISE},
        {"help",no_argument,0,'h'},{0,0,0,0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", opts, NULL)) != -1) {
        switch (c) {
        case O_RATE:  rate_msps = strtod(optarg, NULL); break;
        case O_FTONE: ftone = strtod(optarg, NULL); break;
        case O_AMP:   amp = strtod(optarg, NULL); break;
        case O_SAMP:  samples = strtoull(optarg, NULL, 10); break;
        case O_DROP:  drop_every = strtoull(optarg, NULL, 10); break;
        case O_DUP:   dup_every = strtoull(optarg, NULL, 10); break;
        case O_GARB:  garble_every = strtoull(optarg, NULL, 10); break;
        case O_GLEN:  garble_len = (unsigned)strtoul(optarg, NULL, 10); break;
        case O_FOFF:  freq_ppm = strtod(optarg, NULL); break;
        case O_NOISE: noise = strtod(optarg, NULL); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind < argc) seconds = strtod(argv[optind++], NULL);
    if (optind < argc) { usage(argv[0]); return 2; }

    unsigned samplerate = (unsigned)(rate_msps * 1e6 + 0.5);
    if (samplerate == 0 || rate_msps <= 0 || ftone <= 0) {
        fprintf(stderr, "%s: bad rate/ftone\n", PROG_NAME); return 2;
    }
    if (samples == 0 && seconds > 0)
        samples = (uint64_t)(seconds * (double)samplerate + 0.5);
    int endless = (samples == 0);

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_DFL);     /* reader closing the pipe terminates us */

    double w = 2.0 * M_PI * ftone * (1.0 + freq_ppm / 1e6) / (double)samplerate;
    uint32_t rng = 0x12345678u;
    int16_t *buf = malloc(BLOCK * sizeof(int16_t));
    if (!buf) { fprintf(stderr, "%s: OOM\n", PROG_NAME); return 1; }

    uint64_t n = 0;               /* index into the IDEAL (pre-defect) tone */
    uint64_t emitted = 0;
    uint64_t garble_left = 0;
    fprintf(stderr, "%s: %g MSPS, tone %g MHz%s%s, %s%" PRIu64 " samples\n",
            PROG_NAME, rate_msps, ftone / 1e6,
            freq_ppm != 0.0 ? " (detuned)" : "",
            noise > 0 ? " +noise" : "",
            endless ? "endless (" : "", endless ? 0 : samples);

    while (!g_stop && (endless || emitted < samples)) {
        size_t k = 0;
        while (k < BLOCK && (endless || emitted < samples)) {
            /* defect scheduling on the emitted-sample timeline */
            if (drop_every && emitted && emitted % drop_every == 0) {
                n++;              /* skip one ideal sample -> grid slips forward */
            }
            int dup = (dup_every && emitted && emitted % dup_every == 0);
            if (garble_every && emitted && emitted % garble_every == 0)
                garble_left = garble_len;

            double s = amp * cos(w * (double)n);
            if (noise > 0.0)
                s += ((double)(xorshift(&rng) & 0xffff) / 65535.0 * 2.0 - 1.0) * noise;
            if (garble_left) {    /* overwrite with noise -> amplitude transient */
                s = ((double)(xorshift(&rng) & 0xffff) / 65535.0 * 2.0 - 1.0) * amp;
                garble_left--;
            }
            buf[k++] = clamp16(s);
            emitted++;
            if (!dup) n++;        /* dup: emit same n twice -> grid slips back */
        }
        if (fwrite(buf, sizeof(int16_t), k, stdout) != k) break;  /* pipe closed */
    }
    free(buf);
    return 0;
}
