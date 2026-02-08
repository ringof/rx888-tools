#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// ---- Defaults for your setup ----
static const char *DEFAULT_DATATYPE = "cf32_le";
static const uint64_t DEFAULT_FS_HZ = 33750000ULL;
// 10s * 33.75e6 = 337,500,000 samples
static const uint64_t DEFAULT_SAMPLES_PER_FILE = 337500000ULL;
// 4,194,304 samples * 8 bytes = 32 MiB blocks
static const uint64_t DEFAULT_BLOCK_SAMPLES = 4194304ULL;

// cf32_le = 2 float32 little endian = 8 bytes per complex sample
static const uint64_t BYTES_PER_SAMPLE = 8ULL;

static _Atomic int g_stop = 0;

static void on_signal(int signo) {
  (void)signo;
  g_stop = 1;
}


static void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(1);
}

static void warn_msg(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}
// Escape a C string for JSON string context (minimal but correct for ASCII/UTF-8 input).
// Caller must free returned string.
static char *json_escape(const char *s) {
  if (!s) {
    char *out = (char *)malloc(1);
    if (!out) die("malloc failed");
    out[0] = '\0';
    return out;
  }

  size_t cap = 64;
  size_t len = 0;
  char *out = (char *)malloc(cap);
  if (!out) die("malloc failed");

  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    const unsigned char c = *p;
    const char *rep = NULL;
    char buf[7];

    switch (c) {
      case '\"': rep = "\\\""; break;
      case '\\': rep = "\\\\"; break;
      case '\b': rep = "\\b";  break;
      case '\f': rep = "\\f";  break;
      case '\n': rep = "\\n";  break;
      case '\r': rep = "\\r";  break;
      case '\t': rep = "\\t";  break;
      default:
        if (c < 0x20) {
          // Control chars -> \u00XX
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
          rep = buf;
        }
        break;
    }

    const char *to_add = rep ? rep : (const char[]){(char)c, 0};
    size_t add_len = strlen(to_add);

    if (len + add_len + 1 > cap) {
      while (len + add_len + 1 > cap) cap *= 2;
      out = (char *)realloc(out, cap);
      if (!out) die("realloc failed");
    }
    memcpy(out + len, to_add, add_len);
    len += add_len;
  }

  out[len] = '\0';
  return out;
}


// mkdir -p
static void mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  size_t len = strnlen(path, PATH_MAX - 1);
  if (len == 0 || len >= PATH_MAX) die("mkdir_p: bad path");

  memcpy(tmp, path, len);
  tmp[len] = '\0';

  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        die("mkdir('%s') failed: %s", tmp, strerror(errno));
      }
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    die("mkdir('%s') failed: %s", tmp, strerror(errno));
  }
}

static void format_rfc3339_us_utc(const struct timespec *ts, char *out, size_t out_sz) {
  struct tm g;
  if (!gmtime_r(&ts->tv_sec, &g)) die("gmtime_r failed");
  long us = ts->tv_nsec / 1000;
  // YYYY-mm-ddTHH:MM:SS.ffffffZ
  int n = snprintf(out, out_sz,
                   "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
                   g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                   g.tm_hour, g.tm_min, g.tm_sec, us);
  if (n < 0 || (size_t)n >= out_sz) die("timestamp buffer too small");
}

// Add delta_ns to a timespec (normalized)
static struct timespec timespec_add_ns(struct timespec t, int64_t delta_ns) {
  int64_t sec = (int64_t)t.tv_sec;
  int64_t nsec = (int64_t)t.tv_nsec + delta_ns;

  while (nsec >= 1000000000LL) { nsec -= 1000000000LL; sec += 1; }
  while (nsec < 0)             { nsec += 1000000000LL; sec -= 1; }

  struct timespec out = { .tv_sec = sec, .tv_nsec = nsec };
  return out;
}

// Compute t = t0 + (sample_index * 1e9) / fs, using __int128 for precision.
// __int128 is a GCC/Clang extension (not strict C11) but is necessary here
// to avoid overflow: sample_index * 1e9 exceeds uint64 after ~18.4 seconds.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
static struct timespec t0_plus_samples(const struct timespec *t0, uint64_t sample_index, uint64_t fs_hz) {
  __int128 num = (__int128)sample_index * (__int128)1000000000LL;
  int64_t delta_ns = (int64_t)(num / (__int128)fs_hz);
  return timespec_add_ns(*t0, delta_ns);
}
#pragma GCC diagnostic pop

/* Read with EINTR retry only. Returns a single read()'s result (which may be
   short on pipes). The caller handles short reads via carry-over alignment. */
static ssize_t read_eintr(int fd, void *buf, size_t want) {
  // For pipes: a single read may return less than want; we accept short reads.
  for (;;) {
    ssize_t n = read(fd, buf, want);
    if (n < 0 && errno == EINTR) {
      // Allow Ctrl-C / SIGTERM to break the stream cleanly.
      if (g_stop) return 0;
      continue;
    }
    return n;
  }
}


/* Returns 0 on success, -1 on error (errno set).
 * Pure I/O helper — does not check g_stop.  The caller's outer loop
 * checks g_stop between writes, so any write that starts will finish
 * atomically and sample counters stay accurate. */
static int write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *)buf;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) { errno = EIO; return -1; }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static void maybe_fsync(int fd, bool do_fsync, const char *what) {
  if (!do_fsync) return;
  if (fd < 0) return;
  if (fsync(fd) != 0) {
    fprintf(stderr, "warning: fsync(%s) failed: %s\n", what, strerror(errno));
  }
}


static int open_data_file(const char *outdir, uint64_t index, char *data_path_out, size_t sz) {
  // cap_000000.sigmf-data
  int n = snprintf(data_path_out, sz, "%s/cap_%06" PRIu64 ".sigmf-data", outdir, index);
  if (n < 0 || (size_t)n >= sz) die("data_path buffer too small");

  int fd = open(data_path_out, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) die("open('%s') failed: %s", data_path_out, strerror(errno));
  return fd;
}

static void write_sigmf_meta(const char *outdir,
                             uint64_t index,
                             const char *datatype,
                             uint64_t fs_hz,
                             uint64_t center_freq_hz,
                             bool have_center,
                             const char *desc,
                             const char *start_utc) {
  char tmp_path[PATH_MAX];
  char final_path[PATH_MAX];
  int n;

  n = snprintf(tmp_path, sizeof(tmp_path), "%s/cap_%06" PRIu64 ".sigmf-meta.tmp", outdir, index);
  if (n < 0 || (size_t)n >= sizeof(tmp_path)) die("meta tmp_path buffer too small");
  n = snprintf(final_path, sizeof(final_path), "%s/cap_%06" PRIu64 ".sigmf-meta", outdir, index);
  if (n < 0 || (size_t)n >= sizeof(final_path)) die("meta final_path buffer too small");

  FILE *f = fopen(tmp_path, "wb");
  if (!f) die("fopen('%s') failed: %s", tmp_path, strerror(errno));

  // Minimal SigMF meta (manual JSON). Escape user-provided strings.
  char *desc_esc = json_escape(desc ? desc : "");
  char *dtype_esc = json_escape(datatype ? datatype : "");

  if (!have_center) {
    fprintf(f,
      "{\n"
      "  \"global\": {\n"
      "    \"core:version\": \"1.0.0\",\n"
      "    \"core:datatype\": \"%s\",\n"
      "    \"core:sample_rate\": %" PRIu64 ",\n"
      "    \"core:recorder\": \"iqrecord\",\n"
      "    \"core:description\": \"%s\"\n"
      "  },\n"
      "  \"captures\": [\n"
      "    {\n"
      "      \"core:sample_start\": 0,\n"
      "      \"core:datetime\": \"%s\"\n"
      "    }\n"
      "  ],\n"
      "  \"annotations\": []\n"
      "}\n",
      dtype_esc,
      fs_hz,
      desc_esc,
      start_utc
    );
  } else {
    fprintf(f,
      "{\n"
      "  \"global\": {\n"
      "    \"core:version\": \"1.0.0\",\n"
      "    \"core:datatype\": \"%s\",\n"
      "    \"core:sample_rate\": %" PRIu64 ",\n"
      "    \"core:recorder\": \"iqrecord\",\n"
      "    \"core:description\": \"%s\"\n"
      "  },\n"
      "  \"captures\": [\n"
      "    {\n"
      "      \"core:sample_start\": 0,\n"
      "      \"core:datetime\": \"%s\",\n"
      "      \"core:frequency\": %" PRIu64 "\n"
      "    }\n"
      "  ],\n"
      "  \"annotations\": []\n"
      "}\n",
      dtype_esc,
      fs_hz,
      desc_esc,
      start_utc,
      center_freq_hz
    );
  }

  free(desc_esc);
  free(dtype_esc);
  fclose(f);
  // Atomic rename: file is either complete or absent.
  if (rename(tmp_path, final_path) != 0)
    die("rename('%s' -> '%s') failed: %s", tmp_path, final_path, strerror(errno));
}

static FILE *open_run_json(const char *outdir, const char *created_utc,
                           const char *datatype, uint64_t fs_hz,
                           uint64_t samples_per_file, uint64_t block_samples,
                           const struct timespec *t0_utc) {
  char path[PATH_MAX];
  int n = snprintf(path, sizeof(path), "%s/run.json.tmp", outdir);
  if (n < 0 || (size_t)n >= sizeof(path)) die("run.json.tmp path too small");

  FILE *f = fopen(path, "wb");
  if (!f) die("fopen('%s') failed: %s", path, strerror(errno));

  char t0str[64];
  format_rfc3339_us_utc(t0_utc, t0str, sizeof(t0str));

  // Start the JSON; we’ll keep it simple: write header + open the files array.
  fprintf(f,
    "{\n"
    "  \"schema\": \"iqrecord.run.v1\",\n"
    "  \"created_utc\": \"%s\",\n"
    "  \"stream\": {\n"
    "    \"format\": {\n"
    "      \"datatype\": \"%s\",\n"
    "      \"bytes_per_sample\": %" PRIu64 ",\n"
    "      \"sample_rate_hz\": %" PRIu64 "\n"
    "    },\n"
    "    \"timing\": {\n"
    "      \"clock\": \"real\",\n"
    "      \"t0_mode\": \"first\",\n"
    "      \"t0_utc\": \"%s\",\n"
    "      \"derived_datetime\": true,\n"
    "      \"rfc3339_fractional\": \"microseconds\"\n"
    "    },\n"
    "    \"split\": {\n"
    "      \"samples_per_file\": %" PRIu64 ",\n"
    "      \"seconds_per_file\": %.6f\n"
    "    },\n"
    "    \"io\": {\n"
    "      \"block_samples\": %" PRIu64 ",\n"
    "      \"block_bytes\": %" PRIu64 "\n"
    "    }\n"
    "  },\n"
    "  \"files\": [\n",
    created_utc,
    datatype,
    BYTES_PER_SAMPLE,
    fs_hz,
    t0str,
    samples_per_file,
    (double)samples_per_file / (double)fs_hz,
    block_samples,
    block_samples * BYTES_PER_SAMPLE
  );

  fflush(f);
  return f;
}

static void append_run_json_file(FILE *runf, bool first_entry,
                                uint64_t index,
                                uint64_t start_sample,
                                uint64_t sample_count,
                                const char *start_utc,
                                const char *end_utc,
                                uint64_t data_bytes) {
  fprintf(runf,
    "%s"
    "    {\n"
    "      \"index\": %" PRIu64 ",\n"
    "      \"data\": \"cap_%06" PRIu64 ".sigmf-data\",\n"
    "      \"meta\": \"cap_%06" PRIu64 ".sigmf-meta\",\n"
    "      \"start_sample\": %" PRIu64 ",\n"
    "      \"sample_count\": %" PRIu64 ",\n"
    "      \"start_utc\": \"%s\",\n"
    "      \"end_utc\": \"%s\",\n"
    "      \"data_bytes\": %" PRIu64 "\n"
    "    }",
    first_entry ? "" : ",\n",
    index,
    index,
    index,
    start_sample,
    sample_count,
    start_utc,
    end_utc,
    data_bytes
  );
  fflush(runf);
}

static void finalize_run_json(FILE *runf, const char *outdir,
                             uint64_t files_written,
                             uint64_t samples_written,
                             uint64_t bytes_written) {
  // Close files array, then append accounting summary.
  fprintf(runf,
    "\n"
    "  ],\n"
    "  \"accounting\": {\n"
    "    \"files_written\": %" PRIu64 ",\n"
    "    \"samples_written\": %" PRIu64 ",\n"
    "    \"bytes_written\": %" PRIu64 "\n"
    "  }\n"
    "}\n",
    files_written, samples_written, bytes_written
  );
  fflush(runf);
  fclose(runf);

  // Atomic rename: run.json is either complete or absent.
  char tmp_path[PATH_MAX], final_path[PATH_MAX];
  snprintf(tmp_path, sizeof(tmp_path), "%s/run.json.tmp", outdir);
  snprintf(final_path, sizeof(final_path), "%s/run.json", outdir);
  if (rename(tmp_path, final_path) != 0)
    fprintf(stderr, "iqrecord: rename('%s' -> '%s') failed: %s\n",
            tmp_path, final_path, strerror(errno));
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
      "usage: %s OUTDIR [--freq HZ] [--desc TEXT]\n"
      "MVP defaults: datatype=cf32_le Fs=33750000 samples_per_file=337500000 block_samples=4194304\n",
      argv[0]
    );
    return 1;
  }

  const char *outdir = argv[1];
  uint64_t center_freq = 0;
  bool have_center = false;
  const char *desc = "RX888 DSP output (cf32)";

  bool do_fsync = false;

  // minimal arg parse
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--freq") && i + 1 < argc) {
      center_freq = strtoull(argv[++i], NULL, 10);
      have_center = true;
    } else if (!strcmp(argv[i], "--desc") && i + 1 < argc) {
      desc = argv[++i];
    } else if (!strcmp(argv[i], "--fsync")) {
      do_fsync = true;
    } else {
      die("unknown arg: %s", argv[i]);
    }
  }

  if (isatty(STDIN_FILENO)) {
    fprintf(stderr, "iqrecord: stdin is a TTY; provide input via a pipe or redirection\n");
    return 1;
  }

  mkdir_p(outdir);

  // Handle Ctrl-C / termination for clean finalization.
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  // t0 = time of first read (t0_mode=first)
  struct timespec t0 = {0};
  bool t0_set = false;

  const uint64_t fs_hz = DEFAULT_FS_HZ;
  const uint64_t samples_per_file = DEFAULT_SAMPLES_PER_FILE;
  const uint64_t block_samples = DEFAULT_BLOCK_SAMPLES;
  const size_t block_bytes_want = (size_t)(block_samples * BYTES_PER_SAMPLE);
    // Buffer must hold up to (carry_len bytes from prior read) + (newly read bytes).
  // carry_len is always < BYTES_PER_SAMPLE, so allocate a small explicit headroom.
  const size_t buf_bytes_raw = block_bytes_want + (size_t)BYTES_PER_SAMPLE - 1;
  // C11 requires aligned_alloc size to be a multiple of alignment; round up.
  const size_t buf_bytes = (buf_bytes_raw + 4095) & ~(size_t)4095;

  uint8_t *buf = (uint8_t *)aligned_alloc(4096, buf_bytes);
  if (!buf) die("aligned_alloc failed");

  uint64_t file_index = 0;
  uint64_t samples_in_file = 0;
  uint64_t samples_total = 0;
  uint64_t bytes_total = 0;
  bool write_error = false;

  char created_utc[64];
  {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    format_rfc3339_us_utc(&now, created_utc, sizeof(created_utc));
  }

  // We'll open run.json once t0 is known (after first successful read).
  FILE *runf = NULL;
  bool first_run_entry = true;

  char data_path[PATH_MAX];
  int outfd = -1; // lazy-open only after first successful read

  // Carry-over buffer to handle rare cases where a read ends mid-sample.
  // This makes the recorder robust to abrupt producer shutdowns without corrupting alignment.
  size_t carry_len = 0;

  for (;;) {
    if (g_stop) break;

    // Read into buf after any carried bytes.
    size_t want = block_bytes_want - carry_len;
    ssize_t r = read_eintr(STDIN_FILENO, buf + carry_len, want);
    if (r == 0) break; // EOF (or interrupted via signal)

    if (r < 0) die("read failed: %s", strerror(errno));

    if (!t0_set) {
      clock_gettime(CLOCK_REALTIME, &t0);
      t0_set = true;
      runf = open_run_json(outdir, created_utc, DEFAULT_DATATYPE, fs_hz,
                           samples_per_file, block_samples, &t0);

      if (outfd < 0) {
        outfd = open_data_file(outdir, file_index, data_path, sizeof(data_path));
      }
    }

    // Ensure an output file is open whenever we have aligned data to write.
    // This is required after rotating a file at an exact boundary (no spillover).
    if (outfd < 0) {
      outfd = open_data_file(outdir, file_index, data_path, sizeof(data_path));
    }

    size_t total = carry_len + (size_t)r;
    size_t aligned = total - (total % (size_t)BYTES_PER_SAMPLE);
    size_t rem = total - aligned;

    // If no full samples yet, carry bytes are already at buf[0..total-1]
    // (we read into buf + carry_len). Just update carry_len and retry.
    if (aligned == 0) {
      carry_len = rem;
      continue;
    }

    // Process only the aligned portion.
    uint64_t samples_in_buf = (uint64_t)aligned / BYTES_PER_SAMPLE;
    uint64_t off_samples = 0;

    while (off_samples < samples_in_buf) {
      uint64_t remaining_in_buf = samples_in_buf - off_samples;
      uint64_t remaining_in_file = samples_per_file - samples_in_file;

      uint64_t to_write_samples = remaining_in_buf < remaining_in_file
                                  ? remaining_in_buf : remaining_in_file;
      size_t to_write_bytes = (size_t)(to_write_samples * BYTES_PER_SAMPLE);

      const uint8_t *p = buf + (off_samples * BYTES_PER_SAMPLE);
      if (write_all(outfd, p, to_write_bytes) != 0) {
        fprintf(stderr, "iqrecord: write failed: %s\n", strerror(errno));
        write_error = true;
        g_stop = 1;
        break;
      }

      off_samples += to_write_samples;
      samples_in_file += to_write_samples;
      samples_total += to_write_samples;
      bytes_total += (uint64_t)to_write_bytes;

      if (samples_in_file == samples_per_file) {
        // Close current file and emit metadata.
        if (outfd >= 0) {
          maybe_fsync(outfd, do_fsync, "data");
          close(outfd);
        }
        outfd = -1;

        uint64_t file_start_sample = samples_total - samples_per_file;
        struct timespec t_start = t0_plus_samples(&t0, file_start_sample, fs_hz);
        struct timespec t_end   = t0_plus_samples(&t0, file_start_sample + samples_per_file, fs_hz);

        char start_utc[64], end_utc[64];
        format_rfc3339_us_utc(&t_start, start_utc, sizeof(start_utc));
        format_rfc3339_us_utc(&t_end,   end_utc,   sizeof(end_utc));

        write_sigmf_meta(outdir, file_index, DEFAULT_DATATYPE, fs_hz,
                         center_freq, have_center, desc, start_utc);

        if (runf) {
          append_run_json_file(runf, first_run_entry,
                               file_index,
                               file_start_sample,
                               samples_per_file,
                               start_utc,
                               end_utc,
                               samples_per_file * BYTES_PER_SAMPLE);
          first_run_entry = false;
        }

        // Next file.
        file_index++;
        samples_in_file = 0;

        // Only open the next file if we still have samples to write right now
        // (spillover from the current buffer).
        if (off_samples < samples_in_buf) {
          outfd = open_data_file(outdir, file_index, data_path, sizeof(data_path));
        }
      }
    }

    // Now that aligned data has been written, save any remainder for next read.
    if (rem > 0) {
      memmove(buf, buf + aligned, rem);
      carry_len = rem;
    } else {
      carry_len = 0;
    }
  }

  if (carry_len > 0) {
    warn_msg("Dropping %zu trailing byte(s) (partial sample) at end of stream", carry_len);
  }

  // Close partial last file (if any samples written)
  if (outfd >= 0) {
    maybe_fsync(outfd, do_fsync, "data");
    close(outfd);
  }


  // Emit meta for partial file too (optional; I recommend yes)
  if (t0_set && samples_in_file > 0) {
    uint64_t file_start_sample = samples_total - samples_in_file;
    struct timespec t_start = t0_plus_samples(&t0, file_start_sample, fs_hz);
    struct timespec t_end   = t0_plus_samples(&t0, file_start_sample + samples_in_file, fs_hz);

    char start_utc[64], end_utc[64];
    format_rfc3339_us_utc(&t_start, start_utc, sizeof(start_utc));
    format_rfc3339_us_utc(&t_end,   end_utc,   sizeof(end_utc));

    // meta for partial file_index
    write_sigmf_meta(outdir, file_index, DEFAULT_DATATYPE, fs_hz,
                     center_freq, have_center, desc, start_utc);

    if (runf) {
      append_run_json_file(runf, first_run_entry,
                           file_index,
                           file_start_sample,
                           samples_in_file,
                           start_utc,
                           end_utc,
                           samples_in_file * BYTES_PER_SAMPLE);
      first_run_entry = false;
    }
    file_index++;
  }

  if (runf) {
    finalize_run_json(runf, outdir, file_index, samples_total, bytes_total);
  } else {
    warn_msg("No data read; run.json not written");
  }

  free(buf);
  return write_error ? 1 : 0;
}

