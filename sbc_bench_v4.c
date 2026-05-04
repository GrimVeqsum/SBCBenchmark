#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <locale.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <malloc.h>
#endif

#include "sbc_bench_types.h"
#include "sbc_bench_scenarios.h"
#include "sbc_bench_metrics.h"
#include "sbc_bench_noise.h"

static volatile sig_atomic_t g_stop = 0;

typedef struct
{
  double *v;
  size_t n;
  size_t cap;
} Series;

static void series_init(Series *s, size_t cap)
{
  s->n = 0;
  s->cap = cap;
  s->v = (double *)calloc(cap ? cap : 1, sizeof(double));
}
static void series_push(Series *s, double x)
{
  if (!s || !s->v || s->n >= s->cap)
    return;
  s->v[s->n++] = x;
}
static void series_free(Series *s)
{
  if (!s)
    return;
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}

static void setup_console_encoding(void)
{
  setlocale(LC_ALL, "");
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
}

#ifdef _WIN32
static int posix_memalign_portable(void **memptr, size_t alignment, size_t size)
{
  void *p = _aligned_malloc(size, alignment);
  if (!p)
    return ENOMEM;
  *memptr = p;
  return 0;
}
static void free_aligned_portable(void *p) { _aligned_free(p); }
static int fdatasync_portable(int fd) { return _commit(fd); }
static int mkdir_portable(const char *path, mode_t mode)
{
  (void)mode;
  return _mkdir(path);
}
static void sleep_100ms(void) { Sleep(100); }
static void localtime_r_portable(const time_t *tt, struct tm *out) { localtime_s(out, tt); }
#else
static int posix_memalign_portable(void **memptr, size_t alignment, size_t size) { return posix_memalign(memptr, alignment, size); }
static void free_aligned_portable(void *p) { free(p); }
static int fdatasync_portable(int fd) { return fdatasync(fd); }
static int mkdir_portable(const char *path, mode_t mode) { return mkdir(path, mode); }
static void sleep_100ms(void) { usleep(100000); }
static void localtime_r_portable(const time_t *tt, struct tm *out) { localtime_r(tt, out); }
#endif

static const char *kind_name(WorkKind k)
{
  switch (k)
  {
  case WK_IDLE:
    return "idle";
  case WK_CPU_BURN:
    return "cpu_burn";
  case WK_STORAGE:
    return "storage";
  case WK_PING:
    return "ping";
  case WK_NN:
    return "nn_inference";
  case WK_MEMORY:
    return "memory";
  case WK_JITTER:
    return "jitter";
  default:
    return "unknown";
  }
}

static void on_sig(int sig)
{
  (void)sig;
  g_stop = 1;
}

static double now_sec(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int read_text(const char *path, char *buf, size_t n)
{
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  if (!fgets(buf, (int)n, f))
  {
    fclose(f);
    return -1;
  }
  fclose(f);
  size_t len = strlen(buf);
  while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    buf[--len] = '\0';
  return 0;
}

static double read_first_double(const char *path, double div, double defv)
{
  char b[256];
  if (read_text(path, b, sizeof(b)) != 0)
    return defv;
  char *end = NULL;
  double v = strtod(b, &end);
  if (end == b)
    return defv;
  return v / div;
}

static int join_path(char *dst, size_t dst_sz, const char *dir, const char *name)
{
  if (!dst || !dir || !name || dst_sz == 0)
    return -1;
  int n = snprintf(dst, dst_sz, "%s/%s", dir, name);
  if (n < 0 || (size_t)n >= dst_sz)
    return -1;
  return 0;
}

static int mkdir_p(const char *path)
{
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
    return -1;
  for (char *p = tmp + 1; *p; ++p)
  {
    if (*p == '/')
    {
      *p = '\0';
      if (mkdir_portable(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  if (mkdir_portable(tmp, 0755) != 0 && errno != EEXIST)
    return -1;
  return 0;
}
/* ---------- telemetry ---------- */

static int list_cpu_freq_paths(char paths[MAX_CPUS][PATH_MAX])
{
  int n = 0;
  for (int i = 0; i < MAX_CPUS; ++i)
  {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
    if (access(p, R_OK) == 0)
      strncpy(paths[n++], p, PATH_MAX - 1);
  }
  return n;
}

static double read_max_temp_c(void)
{
  double mx = -1.0;
  for (int i = 0; i < 128; ++i)
  {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", i);
    if (access(p, R_OK) != 0)
      continue;
    double t = read_first_double(p, 1000.0, -1.0);
    if (t > mx)
      mx = t;
  }
  return mx;
}

static double read_avg_cpu_freq_mhz(void)
{
  static char paths[MAX_CPUS][PATH_MAX];
  static int npaths = -1;
  if (npaths < 0)
    npaths = list_cpu_freq_paths(paths);
  if (npaths <= 0)
    return -1.0;
  double sum = 0.0;
  int n = 0;
  for (int i = 0; i < npaths; ++i)
  {
    double v = read_first_double(paths[i], 1000.0, -1.0);
    if (v > 0.0)
    {
      sum += v;
      n++;
    }
  }
  return n ? sum / n : -1.0;
}

static double read_power_w(void)
{
  char p[PATH_MAX];
  for (int i = 0; i < 256; ++i)
  {
    snprintf(p, sizeof(p), "/sys/class/hwmon/hwmon%d/power1_input", i);
    if (access(p, R_OK) == 0)
      return read_first_double(p, 1000000.0, -1.0);
  }
  for (int i = 0; i < 16; ++i)
  {
    snprintf(p, sizeof(p), "/sys/class/power_supply/BAT%d/power_now", i);
    if (access(p, R_OK) == 0)
      return read_first_double(p, 1000000.0, -1.0);
  }
  return -1.0;
}

static double read_psi_avg10(const char *kind)
{
  char p[PATH_MAX], line[512];
  snprintf(p, sizeof(p), "/proc/pressure/%s", kind);
  FILE *f = fopen(p, "r");
  if (!f)
    return -1.0;
  if (!fgets(line, sizeof(line), f))
  {
    fclose(f);
    return -1.0;
  }
  fclose(f);
  char *pos = strstr(line, "avg10=");
  if (!pos)
    return -1.0;
  return atof(pos + 6);
}

static double read_cpu_util_pct(void)
{
  static uint64_t p_idle = 0, p_total = 0;
  char line[512];
  FILE *f = fopen("/proc/stat", "r");
  if (!f)
    return -1.0;
  if (!fgets(line, sizeof(line), f))
  {
    fclose(f);
    return -1.0;
  }
  fclose(f);

  uint64_t u = 0, n = 0, s = 0, id = 0, iw = 0, iq = 0, si = 0, st = 0;
  if (sscanf(line, "cpu %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64,
             &u, &n, &s, &id, &iw, &iq, &si, &st) < 4)
    return -1.0;

  uint64_t idle = id + iw;
  uint64_t total = u + n + s + id + iw + iq + si + st;

  if (p_total == 0)
  {
    p_idle = idle;
    p_total = total;
    return -1.0;
  }

  uint64_t didle = idle - p_idle;
  uint64_t dtotal = total - p_total;
  p_idle = idle;
  p_total = total;
  if (!dtotal)
    return -1.0;
  return 100.0 * (1.0 - (double)didle / (double)dtotal);
}

static double read_mem_used_pct(void)
{
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f)
    return -1.0;
  char line[256];
  double total = 0.0, avail = -1.0;
  while (fgets(line, sizeof(line), f))
  {
    if (sscanf(line, "MemTotal: %lf kB", &total) == 1)
      continue;
    if (sscanf(line, "MemAvailable: %lf kB", &avail) == 1)
      continue;
  }
  fclose(f);
  if (total <= 0.0 || avail < 0.0)
    return -1.0;
  return 100.0 * (1.0 - avail / total);
}

static void *collector_thread(void *arg)
{
  Collector *c = (Collector *)arg;
  while (!c->stop && !g_stop)
  {
    Row r;
    r.ts = now_sec();
    r.temp_c = read_max_temp_c();
    r.cpu_freq_mhz = read_avg_cpu_freq_mhz();
    r.cpu_util_pct = read_cpu_util_pct();
    r.mem_used_pct = read_mem_used_pct();
    r.power_w = read_power_w();
    r.psi_cpu_some_avg10 = read_psi_avg10("cpu");
    r.psi_io_some_avg10 = read_psi_avg10("io");
    r.psi_mem_some_avg10 = read_psi_avg10("memory");

    pthread_mutex_lock(&c->lock);
    if (c->nrows < c->cap)
      c->rows[c->nrows++] = r;
    pthread_mutex_unlock(&c->lock);

    for (int i = 0; i < c->sample_sec * 10 && !c->stop && !g_stop; ++i)
      sleep_100ms();
  }
  return NULL;
}

static void write_telemetry_csv(const Collector *c)
{
  char path[PATH_MAX];
  if (join_path(path, sizeof(path), c->out_dir, "telemetry.csv") != 0)
    return;
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "ts,temp_c,cpu_freq_mhz,cpu_util_pct,mem_used_pct,power_w,psi_cpu_some_avg10,psi_io_some_avg10,psi_mem_some_avg10\n");
  for (size_t i = 0; i < c->nrows; ++i)
  {
    const Row *r = &c->rows[i];
    fprintf(f, "%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            r->ts, r->temp_c, r->cpu_freq_mhz, r->cpu_util_pct, r->mem_used_pct, r->power_w,
            r->psi_cpu_some_avg10, r->psi_io_some_avg10, r->psi_mem_some_avg10);
  }
  fclose(f);
}
/* ---------- CPU / NN / MEMORY / JITTER ---------- */

typedef struct
{
  volatile sig_atomic_t *stop;
  uint64_t *counter;
} BurnArgs;

static void *burn_worker(void *arg)
{
  BurnArgs *a = (BurnArgs *)arg;
  uint64_t c = 0;
  uint32_t x = 1u;
  while (!*(a->stop) && !g_stop)
  {
    for (int i = 0; i < 20000; ++i)
    {
      x = x * 1664525u + 1013904223u;
      c++;
    }
    *(a->counter) = c;
  }
  (void)x;
  return NULL;
}

static double run_cpu_burn(const Step *s, double *window_start_ops, double *window_end_ops, double *degradation_pct)
{
  int n = s->threads > 0 ? s->threads : 1;
  if (n > MAX_CPUS)
    n = MAX_CPUS;

  pthread_t th[MAX_CPUS];
  BurnArgs args[MAX_CPUS];
  uint64_t counters[MAX_CPUS];
  memset(counters, 0, sizeof(counters));

  volatile sig_atomic_t stop = 0;
  double t0 = now_sec();

  for (int i = 0; i < n; ++i)
  {
    args[i].stop = &stop;
    args[i].counter = &counters[i];
    pthread_create(&th[i], NULL, burn_worker, &args[i]);
  }

  int total_ticks = s->duration_sec * 10;
  if (total_ticks < 1)
    total_ticks = 1;
  int mid_tick = total_ticks / 2;

  uint64_t first_half = 0, second_half = 0;

  for (int tick = 0; tick < total_ticks && !g_stop; ++tick)
  {
    sleep_100ms();

    uint64_t cur = 0;
    for (int i = 0; i < n; ++i)
      cur += counters[i];

    if (tick < mid_tick)
      first_half = cur;
    else
      second_half = cur - first_half;
  }

  stop = 1;
  for (int i = 0; i < n; ++i)
    pthread_join(th[i], NULL);

  double elapsed = now_sec() - t0;
  uint64_t total = 0;
  for (int i = 0; i < n; ++i)
    total += counters[i];

  double total_ops = elapsed > 0 ? (double)total / elapsed : 0.0;

  double half_sec = (double)(s->duration_sec) / 2.0;
  if (half_sec <= 0.0)
    half_sec = 1.0;

  *window_start_ops = (double)first_half / half_sec;
  *window_end_ops = (double)second_half / half_sec;
  *degradation_pct = calc_degradation_percent(*window_start_ops, *window_end_ops);

  return total_ops;
}

/* --- NN --- */

typedef struct
{
  volatile sig_atomic_t *stop;
  uint64_t *counter;
  int width;
} NnArgs;

static void *nn_worker(void *arg)
{
  NnArgs *a = (NnArgs *)arg;
  int w = a->width;
  int n = w * w;

  float *m1 = (float *)malloc((size_t)n * sizeof(float));
  float *m2 = (float *)malloc((size_t)n * sizeof(float));
  float *out = (float *)malloc((size_t)n * sizeof(float));
  if (!m1 || !m2 || !out)
  {
    free(m1);
    free(m2);
    free(out);
    return NULL;
  }

  for (int i = 0; i < n; ++i)
  {
    m1[i] = (float)((i * 13 + 7) % 31) / 31.0f;
    m2[i] = (float)((i * 17 + 3) % 37) / 37.0f;
    out[i] = 0.0f;
  }

  uint64_t c = 0;
  while (!*(a->stop) && !g_stop)
  {
    for (int r = 0; r < w; ++r)
    {
      int ro = r * w;
      for (int col = 0; col < w; ++col)
      {
        float acc = 0.0f;
        for (int k = 0; k < w; ++k)
          acc += m1[ro + k] * m2[k * w + col];
        out[ro + col] = acc;
      }
    }
    c++;
    *(a->counter) = c;
  }

  free(m1);
  free(m2);
  free(out);
  return NULL;
}

static double run_nn_inference(const Step *s)
{
  int threads = s->threads > 0 ? s->threads : 1;
  if (threads > MAX_CPUS)
    threads = MAX_CPUS;

  int width = 32;
  if (s->arg)
  {
    int w = atoi(s->arg);
    if (w >= 8 && w <= 256)
      width = w;
  }

  pthread_t th[MAX_CPUS];
  NnArgs args[MAX_CPUS];
  uint64_t counters[MAX_CPUS];
  memset(counters, 0, sizeof(counters));
  volatile sig_atomic_t stop = 0;

  double t0 = now_sec();
  for (int i = 0; i < threads; ++i)
  {
    args[i].stop = &stop;
    args[i].counter = &counters[i];
    args[i].width = width;
    pthread_create(&th[i], NULL, nn_worker, &args[i]);
  }

  for (int i = 0; i < s->duration_sec * 10 && !g_stop; ++i)
    sleep_100ms();

  stop = 1;
  for (int i = 0; i < threads; ++i)
    pthread_join(th[i], NULL);

  double elapsed = now_sec() - t0;
  uint64_t total = 0;
  for (int i = 0; i < threads; ++i)
    total += counters[i];
  return elapsed > 0 ? (double)total / elapsed : 0.0;
}

/* --- MEMORY read/write/copy --- */

static int parse_mem_size(const char *arg, size_t *bytes)
{
  if (!bytes)
    return -1;
  size_t b = 64u * 1024u * 1024u; /* default 64MB */
  if (arg && *arg)
  {
    unsigned long long v = 0;
    char suf = 0;
    if (sscanf(arg, "%llu%c", &v, &suf) >= 1)
    {
      if (suf == 'M' || suf == 'm')
        b = (size_t)v * 1024u * 1024u;
      else if (suf == 'K' || suf == 'k')
        b = (size_t)v * 1024u;
      else
        b = (size_t)v;
    }
  }
  if (b < 1024u * 1024u)
    b = 1024u * 1024u;
  if (b > 512u * 1024u * 1024u)
    b = 512u * 1024u * 1024u; /* safety */
  *bytes = b;
  return 0;
}

static void run_memory_test(const Step *s, double *read_mb_s, double *write_mb_s, double *copy_mb_s)
{
  *read_mb_s = *write_mb_s = *copy_mb_s = -1.0;

  size_t bytes = 0;
  if (parse_mem_size(s->arg, &bytes) != 0)
    return;

  uint8_t *a = NULL, *b = NULL;
  if (posix_memalign_portable((void **)&a, 64, bytes) != 0)
    return;
  if (posix_memalign_portable((void **)&b, 64, bytes) != 0)
  {
    free_aligned_portable(a);
    return;
  }

  memset(a, 0x11, bytes);
  memset(b, 0x22, bytes);

  volatile uint64_t sink = 0;

  /* write */
  double t0 = now_sec();
  for (size_t i = 0; i < bytes; i += 64)
    a[i] = (uint8_t)(i & 0xffu);
  double t1 = now_sec();

  /* read */
  for (size_t i = 0; i < bytes; i += 64)
    sink += a[i];
  double t2 = now_sec();

  /* copy */
  memcpy(b, a, bytes);
  double t3 = now_sec();

  double mb = (double)bytes / (1024.0 * 1024.0);
  if (t1 > t0)
    *write_mb_s = mb / (t1 - t0);
  if (t2 > t1)
    *read_mb_s = mb / (t2 - t1);
  if (t3 > t2)
    *copy_mb_s = mb / (t3 - t2);

  (void)sink;
  free_aligned_portable(a);
  free_aligned_portable(b);
}

/* --- JITTER --- */

static void run_jitter_test(const Step *s,
                            double *j_avg, double *j_p50, double *j_p95, double *j_p99, double *j_max,
                            uint64_t *ov500, uint64_t *ov1000)
{
  *j_avg = *j_p50 = *j_p95 = *j_p99 = *j_max = -1.0;
  *ov500 = *ov1000 = 0;

  int period_us = 1000;
  if (s->arg)
  {
    int p = atoi(s->arg);
    if (p >= 100 && p <= 100000)
      period_us = p;
  }

  int samples = s->duration_sec * 1000000 / period_us;
  if (samples < 100)
    samples = 100;
  if (samples > (int)MAX_STAT_POINTS)
    samples = MAX_STAT_POINTS;

  Series ser;
  series_init(&ser, (size_t)samples);

  struct timespec req;
  req.tv_sec = period_us / 1000000;
  req.tv_nsec = (long)(period_us % 1000000) * 1000L;

  for (int i = 0; i < samples && !g_stop; ++i)
  {
    double t0 = now_sec();
    nanosleep(&req, NULL);
    double t1 = now_sec();
    double actual_us = (t1 - t0) * 1e6;
    double jitter = actual_us - (double)period_us;
    if (jitter < 0)
      jitter = -jitter;
    series_push(&ser, jitter);
  }

  if (ser.n > 0)
  {
    StatsSummary st = stats_from_array(ser.v, ser.n);
    *j_avg = st.avg;
    *j_p50 = st.median;
    *j_p95 = st.p95;
    *j_p99 = st.p99;
    *j_max = st.max;

    uint64_t c500 = 0, c1000 = 0;
    for (size_t i = 0; i < ser.n; ++i)
    {
      if (ser.v[i] > 500.0)
        c500++;
      if (ser.v[i] > 1000.0)
        c1000++;
    }
    *ov500 = c500;
    *ov1000 = c1000;
  }

  series_free(&ser);
}
/* ---------- STORAGE / NETWORK ---------- */

static void storage_write_detail_csv_header(FILE *f)
{
  fprintf(f, "op,lat_us\n");
}

static double run_storage(const Step *s, const char *out_dir,
                          double *iops, double *lat_avg, double *lat_p50,
                          double *lat_p95, double *lat_p99, double *lat_p999,
                          double *lat_max, uint64_t *outliers)
{
  *iops = *lat_avg = *lat_p50 = *lat_p95 = *lat_p99 = *lat_p999 = *lat_max = -1.0;
  *outliers = 0;

  char path[PATH_MAX];
  if (join_path(path, sizeof(path), out_dir, "storage_test.bin") != 0)
    return -1.0;

  int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
  if (fd < 0)
    return -1.0;

  size_t block = 4 * 1024 * 1024;
  if (s->arg && strcmp(s->arg, "1M") == 0)
    block = 1024 * 1024;
  if (s->arg && strcmp(s->arg, "8M") == 0)
    block = 8 * 1024 * 1024;

  char *buf = NULL;
  if (posix_memalign_portable((void **)&buf, 4096, block) != 0 || !buf)
  {
    close(fd);
    return -1.0;
  }
  memset(buf, 0x5A, block);

  char detail_csv[PATH_MAX];
  if (join_path(detail_csv, sizeof(detail_csv), out_dir, "storage_detail.csv") != 0)
  {
    free_aligned_portable(buf);
    close(fd);
    return -1.0;
  }
  FILE *df = fopen(detail_csv, "w");
  if (df)
    storage_write_detail_csv_header(df);

  Series lat;
  series_init(&lat, MAX_STAT_POINTS);

  uint64_t written = 0;
  uint64_t ops = 0;
  double t0 = now_sec();

  while (!g_stop && now_sec() - t0 < s->duration_sec)
  {
    double a = now_sec();
    ssize_t wr = write(fd, buf, block);
    if (wr < 0)
    {
      if (errno == EINTR)
        continue;
      break;
    }
    if (fdatasync_portable(fd) != 0)
      break;
    double b = now_sec();

    double lat_us = (b - a) * 1e6;
    series_push(&lat, lat_us);
    if (df)
      fprintf(df, "write_fsync,%.3f\n", lat_us);

    written += (uint64_t)wr;
    ops++;
  }

  /* metadata ops */
  for (int i = 0; i < 32 && !g_stop; ++i)
  {
    char md[PATH_MAX];
    snprintf(md, sizeof(md), "%s/meta_%d.tmp", out_dir, i);

    double a = now_sec();
    int mfd = open(md, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (mfd >= 0)
    {
      write(mfd, "x", 1);
      close(mfd);
    }
    unlink(md);
    double b = now_sec();

    double lat_us = (b - a) * 1e6;
    series_push(&lat, lat_us);
    if (df)
      fprintf(df, "create_unlink,%.3f\n", lat_us);
    ops++;
  }

  if (df)
    fclose(df);

  double elapsed = now_sec() - t0;

  StatsSummary st = {0};
  if (lat.n > 0)
    st = stats_from_array(lat.v, lat.n);

  if (lat.n > 0)
  {
    *lat_avg = st.avg;
    *lat_p50 = st.median;
    *lat_p95 = st.p95;
    *lat_p99 = st.p99;
    *lat_p999 = st.p999;
    *lat_max = st.max;
    *outliers = st.outliers;
  }

  if (elapsed > 0.0)
  {
    *iops = (double)ops / elapsed;
  }

  series_free(&lat);
  free_aligned_portable(buf);
  close(fd);
  unlink(path);

  if (elapsed <= 0.0)
    return -1.0;
  return ((double)written / (1024.0 * 1024.0)) / elapsed;
}

/* ping parser + extended metrics */
static void run_ping(const Step *s,
                     double *loss_pct, double *p95, double *p99,
                     double *minv, double *avgv, double *maxv,
                     uint64_t *errors,
                     const char *out_dir)
{
  *loss_pct = -1.0;
  *p95 = -1.0;
  *p99 = -1.0;
  *minv = *avgv = *maxv = -1.0;
  *errors = 0;

  char cmd[512];
  const char *host = s->arg ? s->arg : "1.1.1.1";
  snprintf(cmd, sizeof(cmd), "ping -i 0.2 -w %d %s 2>&1", s->duration_sec, host);

  char detail_csv[PATH_MAX];
  FILE *df = NULL;
  if (join_path(detail_csv, sizeof(detail_csv), out_dir, "network_detail.csv") == 0)
  {
    df = fopen(detail_csv, "w");
    if (df)
      fprintf(df, "seq,rtt_ms\n");
  }

  FILE *p = popen(cmd, "r");
  if (!p)
  {
    if (df)
      fclose(df);
    return;
  }

  Series rtts;
  series_init(&rtts, MAX_STAT_POINTS);

  char line[1024];
  int seq = 0;
  while (fgets(line, sizeof(line), p))
  {
    if (strstr(line, "time="))
    {
      char *t = strstr(line, "time=");
      if (t)
      {
        double rtt = atof(t + 5);
        if (rtt > 0.0)
        {
          series_push(&rtts, rtt);
          if (df)
            fprintf(df, "%d,%.3f\n", seq++, rtt);
        }
      }
    }
    if (strstr(line, "packet loss"))
    {
      char *pct = strstr(line, "% packet loss");
      if (pct)
      {
        const char *x = pct;
        while (x > line && ((*(x - 1) >= '0' && *(x - 1) <= '9') || *(x - 1) == '.'))
          x--;
        *loss_pct = atof(x);
      }
    }
    if (strstr(line, "unknown host") || strstr(line, "Destination Host Unreachable"))
      (*errors)++;
  }

  pclose(p);
  if (df)
    fclose(df);

  if (rtts.n > 0)
  {
    StatsSummary st = stats_from_array(rtts.v, rtts.n);
    *p95 = st.p95;
    *p99 = st.p99;
    *minv = stats_percentile(rtts.v, rtts.n, 0.0);
    *avgv = st.avg;
    *maxv = st.max;
  }

  series_free(&rtts);
}

/* ---------- per-test CSV ---------- */

static void write_cpu_csv(const char *out_dir, const StepResult *res, int nres)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), out_dir, "cpu.csv") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;
  fprintf(f, "step,ops_per_sec,window_start_ops,window_end_ops,degradation_pct\n");
  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind != WK_CPU_BURN)
      continue;
    fprintf(f, "%s,%.3f,%.3f,%.3f,%.3f\n",
            res[i].step->name,
            res[i].ops_per_sec,
            res[i].ops_window_start,
            res[i].ops_window_end,
            res[i].cpu_degradation_pct);
  }
  fclose(f);
}

static void write_memory_csv(const char *out_dir, const StepResult *res, int nres)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), out_dir, "memory.csv") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;
  fprintf(f, "step,read_mb_s,write_mb_s,copy_mb_s\n");
  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind != WK_MEMORY)
      continue;
    fprintf(f, "%s,%.3f,%.3f,%.3f\n",
            res[i].step->name,
            res[i].mem_read_mb_s,
            res[i].mem_write_mb_s,
            res[i].mem_copy_mb_s);
  }
  fclose(f);
}

static void write_storage_csv(const char *out_dir, const StepResult *res, int nres)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), out_dir, "storage.csv") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;
  fprintf(f, "step,mb_s,iops,lat_avg_us,lat_p50_us,lat_p95_us,lat_p99_us,lat_p999_us,lat_max_us,outliers\n");
  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind != WK_STORAGE)
      continue;
    fprintf(f, "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu64 "\n",
            res[i].step->name,
            res[i].throughput_mb_s,
            res[i].storage_iops,
            res[i].storage_lat_avg_us,
            res[i].storage_lat_p50_us,
            res[i].storage_lat_p95_us,
            res[i].storage_lat_p99_us,
            res[i].storage_lat_p999_us,
            res[i].storage_lat_max_us,
            res[i].storage_outliers);
  }
  fclose(f);
}
static void write_network_csv(const char *out_dir, const StepResult *res, int nres)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), out_dir, "network.csv") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;
  fprintf(f, "step,min_ms,avg_ms,max_ms,p95_ms,p99_ms,loss_pct,errors\n");
  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind != WK_PING)
      continue;
    fprintf(f, "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu64 "\n",
            res[i].step->name,
            res[i].ping_min_ms,
            res[i].ping_avg_ms,
            res[i].ping_max_ms,
            res[i].ping_p95_ms,
            res[i].ping_p99_ms,
            res[i].packet_loss_pct,
            res[i].ping_errors);
  }
  fclose(f);
}

static void write_jitter_csv(const char *out_dir, const StepResult *res, int nres)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), out_dir, "jitter.csv") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;
  fprintf(f, "step,avg_us,p50_us,p95_us,p99_us,max_us,over_500us,over_1000us\n");
  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind != WK_JITTER)
      continue;
    fprintf(f, "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu64 ",%" PRIu64 "\n",
            res[i].step->name,
            res[i].jitter_avg_us,
            res[i].jitter_p50_us,
            res[i].jitter_p95_us,
            res[i].jitter_p99_us,
            res[i].jitter_max_us,
            res[i].jitter_over_500us,
            res[i].jitter_over_1000us);
  }
  fclose(f);
}

/* ---------- run metadata ---------- */

static void write_run_status(const char *run_dir, const char *status, const char *message)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), run_dir, "run_status.json") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;
  fprintf(f, "{\n  \"status\": \"%s\",\n  \"message\": \"%s\"\n}\n",
          status ? status : "unknown",
          message ? message : "");
  fclose(f);
}

static void write_scenario_json(const char *run_dir, const Scenario *sc)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), run_dir, "scenario.json") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;

  fprintf(f, "{\n");
  fprintf(f, "  \"name\": \"%s\",\n", sc->name ? sc->name : "unknown");
  fprintf(f, "  \"description\": \"%s\",\n", sc->description ? sc->description : "");
  fprintf(f, "  \"sample_sec\": %d,\n", sc->sample_sec);
  fprintf(f, "  \"noise_mode\": %d,\n", (int)sc->noise_mode);
  fprintf(f, "  \"steps\": [\n");

  for (int i = 0; i < sc->step_count; ++i)
  {
    const Step *st = &sc->steps[i];
    fprintf(f, "    {\"name\":\"%s\",\"kind\":\"%s\",\"duration_sec\":%d,\"threads\":%d,\"arg\":\"%s\"}%s\n",
            st->name ? st->name : "",
            kind_name(st->kind),
            st->duration_sec,
            st->threads,
            st->arg ? st->arg : "",
            (i + 1 < sc->step_count) ? "," : "");
  }

  fprintf(f, "  ]\n}\n");
  fclose(f);
}

static void write_system_info(const char *run_dir)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), run_dir, "system_info.json") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;

  char host[256] = "unknown";
  gethostname(host, sizeof(host) - 1);

  char kernel[512] = "unknown";
  FILE *vf = fopen("/proc/version", "r");
  if (vf)
  {
    if (fgets(kernel, sizeof(kernel), vf))
    {
      size_t len = strlen(kernel);
      while (len > 0 && (kernel[len - 1] == '\n' || kernel[len - 1] == '\r'))
        kernel[--len] = '\0';
    }
    fclose(vf);
  }

  fprintf(f, "{\n");
  fprintf(f, "  \"hostname\": \"%s\",\n", host);
  fprintf(f, "  \"kernel\": \"%s\"\n", kernel);
  fprintf(f, "}\n");
  fclose(f);
}

/* ---------- metrics/report ---------- */

static void write_metrics_json(const char *run_dir, const Scenario *sc, const Collector *c, const StepResult *res, int nres)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), run_dir, "metrics.json") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;

  Series freq, temp;
  series_init(&freq, c->nrows);
  series_init(&temp, c->nrows);

  for (size_t i = 0; i < c->nrows; ++i)
  {
    if (c->rows[i].cpu_freq_mhz > 0)
      series_push(&freq, c->rows[i].cpu_freq_mhz);
    if (c->rows[i].temp_c > 0)
      series_push(&temp, c->rows[i].temp_c);
  }

  int throttle_hint = 0;
  if (freq.n > 2 && temp.n > 2)
    throttle_hint = detect_freq_drop_with_temp_rise(freq.v, temp.v, freq.n < temp.n ? freq.n : temp.n, 10.0, 5.0);

  double cpu_avg = -1.0, nn_avg = -1.0, mem_copy_avg = -1.0, storage_avg = -1.0, ping_p95_avg = -1.0, jitter_p99_avg = -1.0;
  int c_cpu = 0, c_nn = 0, c_mem = 0, c_st = 0, c_net = 0, c_jit = 0;

  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind == WK_CPU_BURN && res[i].ops_per_sec > 0)
    {
      cpu_avg += (cpu_avg < 0 ? 1 : 0);
      cpu_avg = (cpu_avg < 0 ? 0 : cpu_avg) + res[i].ops_per_sec;
      c_cpu++;
    }
    if (res[i].step->kind == WK_NN && res[i].nn_inf_per_sec > 0)
    {
      nn_avg += (nn_avg < 0 ? 1 : 0);
      nn_avg = (nn_avg < 0 ? 0 : nn_avg) + res[i].nn_inf_per_sec;
      c_nn++;
    }
    if (res[i].step->kind == WK_MEMORY && res[i].mem_copy_mb_s > 0)
    {
      mem_copy_avg += (mem_copy_avg < 0 ? 1 : 0);
      mem_copy_avg = (mem_copy_avg < 0 ? 0 : mem_copy_avg) + res[i].mem_copy_mb_s;
      c_mem++;
    }
    if (res[i].step->kind == WK_STORAGE && res[i].throughput_mb_s > 0)
    {
      storage_avg += (storage_avg < 0 ? 1 : 0);
      storage_avg = (storage_avg < 0 ? 0 : storage_avg) + res[i].throughput_mb_s;
      c_st++;
    }
    if (res[i].step->kind == WK_PING && res[i].ping_p95_ms > 0)
    {
      ping_p95_avg += (ping_p95_avg < 0 ? 1 : 0);
      ping_p95_avg = (ping_p95_avg < 0 ? 0 : ping_p95_avg) + res[i].ping_p95_ms;
      c_net++;
    }
    if (res[i].step->kind == WK_JITTER && res[i].jitter_p99_us > 0)
    {
      jitter_p99_avg += (jitter_p99_avg < 0 ? 1 : 0);
      jitter_p99_avg = (jitter_p99_avg < 0 ? 0 : jitter_p99_avg) + res[i].jitter_p99_us;
      c_jit++;
    }
  }

  if (c_cpu)
    cpu_avg /= c_cpu;
  if (c_nn)
    nn_avg /= c_nn;
  if (c_mem)
    mem_copy_avg /= c_mem;
  if (c_st)
    storage_avg /= c_st;
  if (c_net)
    ping_p95_avg /= c_net;
  if (c_jit)
    jitter_p99_avg /= c_jit;

  fprintf(f, "{\n");
  fprintf(f, "  \"scenario\": \"%s\",\n", sc->name ? sc->name : "unknown");
  fprintf(f, "  \"cpu_ops_avg\": %.3f,\n", cpu_avg);
  fprintf(f, "  \"nn_inf_per_sec_avg\": %.3f,\n", nn_avg);
  fprintf(f, "  \"mem_copy_mb_s_avg\": %.3f,\n", mem_copy_avg);
  fprintf(f, "  \"storage_mb_s_avg\": %.3f,\n", storage_avg);
  fprintf(f, "  \"ping_p95_ms_avg\": %.3f,\n", ping_p95_avg);
  fprintf(f, "  \"jitter_p99_us_avg\": %.3f,\n", jitter_p99_avg);
  fprintf(f, "  \"throttle_hint\": %d\n", throttle_hint);
  fprintf(f, "}\n");
  fclose(f);

  series_free(&freq);
  series_free(&temp);
}

static void write_report_md(const char *run_dir, const Scenario *sc, const Collector *c, const StepResult *res, int nres)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), run_dir, "report.md") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;

  fprintf(f, "# SBC Benchmark Report\n\n");
  fprintf(f, "- Scenario: **%s**\n", sc->name ? sc->name : "unknown");
  fprintf(f, "- Description: %s\n", sc->description ? sc->description : "");
  fprintf(f, "- Samples: %zu\n", c->nrows);
  fprintf(f, "- Tests executed: %d\n\n", nres);

  fprintf(f, "## Steps\n\n");
  fprintf(f, "| Step | Kind | CPU ops/s | NN inf/s | Mem copy MB/s | Storage MB/s | Net p95 ms | Jitter p99 us |\n");
  fprintf(f, "|---|---|---:|---:|---:|---:|---:|---:|\n");
  for (int i = 0; i < nres; ++i)
  {
    fprintf(f, "| %s | %s | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f |\n",
            res[i].step->name,
            kind_name(res[i].step->kind),
            res[i].ops_per_sec,
            res[i].nn_inf_per_sec,
            res[i].mem_copy_mb_s,
            res[i].throughput_mb_s,
            res[i].ping_p95_ms,
            res[i].jitter_p99_us);
  }

  fprintf(f, "\n## Interpretation\n\n");
  fprintf(f, "- Более высокие CPU/NN/MEM/STORAGE показатели — лучше.\n");
  fprintf(f, "- Более низкие latency/jitter/packet loss — лучше.\n");
  fprintf(f, "- Для длительных тестов анализируй деградацию CPU и рост температуры.\n");
  fclose(f);
}
/* ---------- benchmark orchestration ---------- */

static int run_benchmark(Scenario sc, double duration_scale, int replace_latest)
{
  g_stop = 0;

  int total_duration = 0;
  for (int i = 0; i < sc.step_count; ++i)
    total_duration += sc.steps[i].duration_sec;
  if (total_duration >= 900)
    fprintf(stdout, "[INFO] Длительный тест: %d sec (~%.1f min)\n", total_duration, total_duration / 60.0);

  if (duration_scale != 1.0)
  {
    for (int i = 0; i < sc.step_count; ++i)
    {
      int scaled = (int)llround((double)sc.steps[i].duration_sec * duration_scale);
      if (scaled < 1)
        scaled = 1;
      sc.steps[i].duration_sec = scaled;
    }
  }

  print_execution_plan(&sc);

  time_t tt = time(NULL);
  struct tm tm;
  localtime_r_portable(&tt, &tm);

  char run_dir[PATH_MAX];
  if (replace_latest)
    snprintf(run_dir, sizeof(run_dir), "runs_v4_c/%s_latest", sc.name);
  else
    snprintf(run_dir, sizeof(run_dir), "runs_v4_c/%s_%04d%02d%02d_%02d%02d%02d",
             sc.name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

  if (mkdir_p(run_dir) != 0)
  {
    fprintf(stderr, "failed to create run directory: %s\n", run_dir);
    return 1;
  }

  write_run_status(run_dir, "created", "run directory created");
  write_scenario_json(run_dir, &sc);
  write_system_info(run_dir);

  Collector c;
  memset(&c, 0, sizeof(c));
  c.cap = MAX_ROWS;
  c.rows = (Row *)calloc(c.cap, sizeof(Row));
  if (!c.rows)
  {
    fprintf(stderr, "failed to allocate telemetry buffer\n");
    return 1;
  }
  c.sample_sec = sc.sample_sec > 0 ? sc.sample_sec : 1;
  snprintf(c.out_dir, sizeof(c.out_dir), "%s", run_dir);
  pthread_mutex_init(&c.lock, NULL);

  pthread_t th;
  pthread_create(&th, NULL, collector_thread, &c);
  write_run_status(run_dir, "running", "telemetry started");

  NoiseContext *noise = noise_start(sc.noise_mode, 1, run_dir);

  StepResult results[MAX_STEPS];
  int nres = 0;
  memset(results, 0, sizeof(results));

  for (int i = 0; i < sc.step_count && !g_stop; ++i)
  {
    const Step *st = &sc.steps[i];
    StepResult r;
    memset(&r, 0, sizeof(r));
    r.step = st;

    fprintf(stdout, "[STEP] %s (%d sec)\n", st->name, st->duration_sec);

    if (st->kind == WK_IDLE)
    {
      for (int k = 0; k < st->duration_sec * 10 && !g_stop; ++k)
        sleep_100ms();
    }
    else if (st->kind == WK_CPU_BURN)
    {
      r.ops_per_sec = run_cpu_burn(st, &r.ops_window_start, &r.ops_window_end, &r.cpu_degradation_pct);
    }
    else if (st->kind == WK_STORAGE)
    {
      r.throughput_mb_s = run_storage(st, run_dir,
                                      &r.storage_iops,
                                      &r.storage_lat_avg_us,
                                      &r.storage_lat_p50_us,
                                      &r.storage_lat_p95_us,
                                      &r.storage_lat_p99_us,
                                      &r.storage_lat_p999_us,
                                      &r.storage_lat_max_us,
                                      &r.storage_outliers);
    }
    else if (st->kind == WK_PING)
    {
      run_ping(st,
               &r.packet_loss_pct,
               &r.ping_p95_ms,
               &r.ping_p99_ms,
               &r.ping_min_ms,
               &r.ping_avg_ms,
               &r.ping_max_ms,
               &r.ping_errors,
               run_dir);
    }
    else if (st->kind == WK_NN)
    {
      r.nn_inf_per_sec = run_nn_inference(st);
    }
    else if (st->kind == WK_MEMORY)
    {
      run_memory_test(st, &r.mem_read_mb_s, &r.mem_write_mb_s, &r.mem_copy_mb_s);
    }
    else if (st->kind == WK_JITTER)
    {
      run_jitter_test(st,
                      &r.jitter_avg_us,
                      &r.jitter_p50_us,
                      &r.jitter_p95_us,
                      &r.jitter_p99_us,
                      &r.jitter_max_us,
                      &r.jitter_over_500us,
                      &r.jitter_over_1000us);
    }

    results[nres++] = r;
  }

  if (noise)
    noise_stop(noise);

  c.stop = 1;
  pthread_join(th, NULL);

  write_telemetry_csv(&c);
  write_cpu_csv(run_dir, results, nres);
  write_memory_csv(run_dir, results, nres);
  write_storage_csv(run_dir, results, nres);
  write_network_csv(run_dir, results, nres);
  write_jitter_csv(run_dir, results, nres);
  write_metrics_json(run_dir, &sc, &c, results, nres);
  write_report_md(run_dir, &sc, &c, results, nres);

  write_run_status(run_dir, g_stop ? "interrupted" : "completed", g_stop ? "stopped" : "ok");

  free(c.rows);
  fprintf(stdout, "Done. Results: %s\n", run_dir);
  return 0;
}

/* ---------- analyze latest ---------- */

static int find_latest_run_dir(char *out, size_t out_sz)
{
  DIR *d = opendir("runs_v4_c");
  if (!d)
    return -1;

  time_t best_time = 0;
  char best_path[PATH_MAX] = {0};
  struct dirent *ent;

  while ((ent = readdir(d)) != NULL)
  {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;

    char path[PATH_MAX], metrics[PATH_MAX];
    snprintf(path, sizeof(path), "runs_v4_c/%s", ent->d_name);
    if (join_path(metrics, sizeof(metrics), path, "metrics.json") != 0)
      continue;

    struct stat st;
    if (stat(metrics, &st) != 0)
      continue;
    if (st.st_mtime > best_time)
    {
      best_time = st.st_mtime;
      snprintf(best_path, sizeof(best_path), "%s", path);
    }
  }
  closedir(d);

  if (best_time == 0)
    return -1;
  snprintf(out, out_sz, "%s", best_path);
  return 0;
}

static void print_file_text(const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f)
  {
    fprintf(stdout, "Cannot open %s\n", path);
    return;
  }
  char line[1024];
  while (fgets(line, sizeof(line), f))
    fputs(line, stdout);
  fclose(f);
}

static void analyze_latest_run(void)
{
  char latest[PATH_MAX];
  if (find_latest_run_dir(latest, sizeof(latest)) != 0)
  {
    fprintf(stdout, "No runs found under runs_v4_c\n");
    return;
  }

  char metrics_path[PATH_MAX];
  if (join_path(metrics_path, sizeof(metrics_path), latest, "metrics.json") != 0)
    return;

  fprintf(stdout, "\n=== Latest run: %s ===\n", latest);
  print_file_text(metrics_path);
  fprintf(stdout, "\n");
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
  setup_console_encoding();
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  if (argc >= 2 && (strcmp(argv[1], "--list-scenarios") == 0 || strcmp(argv[1], "-l") == 0))
  {
    print_scenario_catalog();
    return 0;
  }
  if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0))
  {
    print_scenario_help(argv[0]);
    return 0;
  }

  if (argc == 1 || (argc >= 2 && strcmp(argv[1], "--menu") == 0))
  {
    while (1)
    {
      char chosen[64] = {0};
      double scale = 1.0;
      int use_custom = 0;
      int replace_latest = 0;

      int menu_status = show_interactive_menu(chosen, &scale, &use_custom, &replace_latest);
      if (menu_status <= 0)
        return 0;

      if (strcmp(chosen, "__analyze__") == 0)
      {
        analyze_latest_run();
        continue;
      }

      if (strcmp(chosen, "__all__") == 0)
      {
        const char *all[] = {"baseline", "long_soak", "server_gateway", "iot", "embedded", "neural_host"};
        int n = (int)(sizeof(all) / sizeof(all[0]));
        for (int i = 0; i < n; ++i)
        {
          Scenario sc = scenario_from_name(all[i]);
          int rc = run_benchmark(sc, scale, replace_latest);
          if (rc != 0 || g_stop)
            return rc;
        }
        continue;
      }

      Scenario sc = use_custom ? build_custom_scenario_from_prompt() : scenario_from_name(chosen);
      int rc = run_benchmark(sc, scale, replace_latest);
      if (rc != 0 || g_stop)
        return rc;
    }
  }

  const char *scenario_name = argv[1];
  if (!is_valid_scenario_name(scenario_name))
  {
    fprintf(stderr, "Unknown scenario: %s\n\n", scenario_name);
    print_scenario_help(argv[0]);
    return 2;
  }

  double duration_scale = (argc >= 3) ? parse_duration_scale(argv[2], 1.0) : 1.0;
  Scenario sc = scenario_from_name(scenario_name);
  return run_benchmark(sc, duration_scale, 0);
}