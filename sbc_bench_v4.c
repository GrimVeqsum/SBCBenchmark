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
#include "sbc_bench_coordinator.h"

static volatile sig_atomic_t g_stop = 0;
static RunMessages g_run_msgs;

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

static void add_warning(const char *msg)
{
  if (!msg || !*msg)
    return;
  if (g_run_msgs.warning_count >= MAX_WARNINGS)
    return;
  snprintf(g_run_msgs.warnings[g_run_msgs.warning_count], sizeof(g_run_msgs.warnings[g_run_msgs.warning_count]), "%s", msg);
  g_run_msgs.warning_count++;
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

static void detect_telemetry_channel_warnings(void)
{
  int thermal_ok = 0;
  for (int i = 0; i < 128; ++i)
  {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", i);
    if (access(p, R_OK) == 0)
    {
      thermal_ok = 1;
      break;
    }
  }
  if (!thermal_ok)
    add_warning("telemetry: thermal zones are unavailable");

  char paths[MAX_CPUS][PATH_MAX];
  if (list_cpu_freq_paths(paths) <= 0)
    add_warning("telemetry: cpu frequency channels are unavailable");

  if (access("/proc/pressure/cpu", R_OK) != 0)
    add_warning("telemetry: PSI cpu is unavailable");
  if (access("/proc/pressure/io", R_OK) != 0)
    add_warning("telemetry: PSI io is unavailable");
  if (access("/proc/pressure/memory", R_OK) != 0)
    add_warning("telemetry: PSI memory is unavailable");
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

  for (int i = 0; i < n; ++i)
  {
    args[i].stop = &stop;
    args[i].counter = &counters[i];
    pthread_create(&th[i], NULL, burn_worker, &args[i]);
  }

  double t0 = now_sec();
  double mid = t0 + s->duration_sec * 0.5;
  uint64_t mid_ops = 0, end_ops = 0;

  while (!g_stop && now_sec() - t0 < s->duration_sec)
  {
    uint64_t sum = 0;
    for (int i = 0; i < n; ++i)
      sum += counters[i];

    if (now_sec() < mid)
      mid_ops = sum;
    end_ops = sum;

    sleep_100ms();
  }

  stop = 1;
  for (int i = 0; i < n; ++i)
    pthread_join(th[i], NULL);

  double elapsed = now_sec() - t0;
  if (elapsed <= 0)
    elapsed = 1e-6;

  const double total_ops = (double)end_ops;
  const double ops_sec = total_ops / elapsed;

  const double first_window = (double)mid_ops / (elapsed * 0.5 > 0 ? elapsed * 0.5 : 1e-6);
  const double second_window = (double)(end_ops - mid_ops) / (elapsed * 0.5 > 0 ? elapsed * 0.5 : 1e-6);

  *window_start_ops = first_window;
  *window_end_ops = second_window;
  *degradation_pct = calc_degradation_percent(first_window, second_window);

  return ops_sec;
}
static double run_nn_inference(const Step *s)
{
  int n = s->threads > 0 ? s->threads : 1;
  int dim = 32;
  if (s->arg)
  {
    int v = atoi(s->arg);
    if (v >= 8 && v <= 128)
      dim = v;
  }

  size_t sz = (size_t)dim * (size_t)dim;
  float *A = (float *)malloc(sz * sizeof(float));
  float *B = (float *)malloc(sz * sizeof(float));
  float *C = (float *)malloc(sz * sizeof(float));
  if (!A || !B || !C)
  {
    free(A);
    free(B);
    free(C);
    return -1.0;
  }

  for (size_t i = 0; i < sz; ++i)
  {
    A[i] = (float)((i % 13) * 0.1);
    B[i] = (float)((i % 7) * 0.2);
    C[i] = 0.0f;
  }

  double t0 = now_sec();
  uint64_t iters = 0;

  while (!g_stop && now_sec() - t0 < s->duration_sec)
  {
    for (int t = 0; t < n; ++t)
    {
      for (int i = 0; i < dim; ++i)
      {
        for (int j = 0; j < dim; ++j)
        {
          float acc = 0.0f;
          for (int k = 0; k < dim; ++k)
            acc += A[(size_t)i * dim + k] * B[(size_t)k * dim + j];
          C[(size_t)i * dim + j] = acc;
        }
      }
    }
    iters++;
  }

  double elapsed = now_sec() - t0;
  free(A);
  free(B);
  free(C);

  if (elapsed <= 0.0)
    return -1.0;
  return (double)iters / elapsed;
}

static size_t parse_mem_bytes(const char *arg)
{
  if (!arg || !*arg)
    return 64u * 1024u * 1024u;
  char *end = NULL;
  long long v = strtoll(arg, &end, 10);
  if (end == arg || v <= 0)
    return 64u * 1024u * 1024u;

  size_t mul = 1;
  if (*end == 'K' || *end == 'k')
    mul = 1024u;
  else if (*end == 'M' || *end == 'm')
    mul = 1024u * 1024u;
  else if (*end == 'G' || *end == 'g')
    mul = 1024u * 1024u * 1024u;

  size_t out = (size_t)v * mul;
  if (out < 1024u * 1024u)
    out = 1024u * 1024u;
  if (out > 1024u * 1024u * 1024u)
    out = 1024u * 1024u * 1024u;
  return out;
}

static void run_memory_test(const Step *s, double *read_mb_s, double *write_mb_s, double *copy_mb_s)
{
  *read_mb_s = *write_mb_s = *copy_mb_s = -1.0;

  const size_t nbytes = parse_mem_bytes(s->arg);
  const size_t n = nbytes / sizeof(uint64_t);

  uint64_t *a = NULL, *b = NULL;
  if (posix_memalign_portable((void **)&a, 64, nbytes) != 0 ||
      posix_memalign_portable((void **)&b, 64, nbytes) != 0 ||
      !a || !b)
  {
    if (a)
      free_aligned_portable(a);
    if (b)
      free_aligned_portable(b);
    return;
  }

  for (size_t i = 0; i < n; ++i)
  {
    a[i] = (uint64_t)i;
    b[i] = 0;
  }

  double t0 = now_sec();
  uint64_t rounds = 0;

  while (!g_stop && now_sec() - t0 < s->duration_sec)
  {
    for (size_t i = 0; i < n; ++i)
      a[i] = a[i] * 1664525u + 1013904223u;
    rounds++;
  }
  double t1 = now_sec();

  volatile uint64_t sink = 0;
  double r0 = now_sec();
  for (size_t i = 0; i < n; ++i)
    sink += a[i];
  double r1 = now_sec();

  double c0 = now_sec();
  memcpy(b, a, nbytes);
  double c1 = now_sec();

  (void)sink;

  double elapsed_write = t1 - t0;
  double elapsed_read = r1 - r0;
  double elapsed_copy = c1 - c0;

  double total_mb = ((double)nbytes * (double)rounds) / (1024.0 * 1024.0);

  if (elapsed_write > 0)
    *write_mb_s = total_mb / elapsed_write;
  if (elapsed_read > 0)
    *read_mb_s = ((double)nbytes / (1024.0 * 1024.0)) / elapsed_read;
  if (elapsed_copy > 0)
    *copy_mb_s = ((double)nbytes / (1024.0 * 1024.0)) / elapsed_copy;

  free_aligned_portable(a);
  free_aligned_portable(b);
}

static void run_jitter_test(const Step *s,
                            double *j_avg, double *j_p50, double *j_p95, double *j_p99, double *j_max,
                            uint64_t *ov500, uint64_t *ov1000)
{
  *j_avg = *j_p50 = *j_p95 = *j_p99 = *j_max = -1.0;
  *ov500 = *ov1000 = 0;

  int period_us = 1000;
  if (s->arg)
  {
    int v = atoi(s->arg);
    if (v >= 100 && v <= 100000)
      period_us = v;
  }

  int samples = s->duration_sec * 1000000 / period_us;
  if (samples < 10)
    samples = 10;
  if (samples > (int)MAX_STAT_POINTS)
    samples = (int)MAX_STAT_POINTS;

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
  uint64_t read_bytes = 0;
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

    if (lseek(fd, 0, SEEK_SET) >= 0)
    {
      double ra = now_sec();
      ssize_t rd = read(fd, buf, block);
      double rb = now_sec();
      if (rd > 0)
      {
        read_bytes += (uint64_t)rd;
        double lat_r_us = (rb - ra) * 1e6;
        series_push(&lat, lat_r_us);
        if (df)
          fprintf(df, "seq_read,%.3f\n", lat_r_us);
        ops++;
      }
    }

    if (written > block)
    {
      off_t max_off = (off_t)(written - block);
      off_t off = (off_t)((uint64_t)rand() % (uint64_t)(max_off + 1));
      off = (off / 4096) * 4096;
      double rra = now_sec();
      ssize_t rrd = pread(fd, buf, block, off);
      double rrb = now_sec();
      if (rrd > 0)
      {
        read_bytes += (uint64_t)rrd;
        double lat_rr_us = (rrb - rra) * 1e6;
        series_push(&lat, lat_rr_us);
        if (df)
          fprintf(df, "random_read,%.3f\n", lat_rr_us);
        ops++;
      }
    }
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

  for (int i = 0; i < 16 && !g_stop; ++i)
  {
    char dpath[PATH_MAX];
    snprintf(dpath, sizeof(dpath), "%s/bench_dir_%d", out_dir, i);
    double a = now_sec();
    int mk = mkdir_portable(dpath, 0755);
    int rm = (mk == 0) ? rmdir(dpath) : -1;
    double b = now_sec();
    if (mk == 0 && rm == 0)
    {
      double lat_us = (b - a) * 1e6;
      series_push(&lat, lat_us);
      if (df)
        fprintf(df, "mkdir_rmdir,%.3f\n", lat_us);
      ops++;
    }
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
  return ((double)(written + read_bytes) / (1024.0 * 1024.0)) / elapsed;
}

/* ping parser + extended metrics */
static void run_ping(const Step *s,
                     double *loss_pct, double *p95, double *p99,
                     double *minv, double *avgv, double *maxv,
                     uint64_t *errors, const char *out_dir)
{
  *loss_pct = *p95 = *p99 = *minv = *avgv = *maxv = -1.0;
  *errors = 0;

  const char *host = (s->arg && *s->arg) ? s->arg : "1.1.1.1";

  char cmd[512];
  snprintf(cmd, sizeof(cmd), "ping -i 0.2 -w %d %s 2>&1", s->duration_sec, host);

  FILE *p = popen(cmd, "r");
  if (!p)
  {
    *errors = 1;
    return;
  }

  Series rtts;
  series_init(&rtts, MAX_STAT_POINTS);

  char detail_csv[PATH_MAX];
  FILE *df = NULL;
  if (out_dir && join_path(detail_csv, sizeof(detail_csv), out_dir, "network_detail.csv") == 0)
  {
    df = fopen(detail_csv, "w");
    if (df)
      fprintf(df, "rtt_ms\n");
  }

  char line[1024];
  while (fgets(line, sizeof(line), p))
  {
    if (strstr(line, "time="))
    {
      char *x = strstr(line, "time=");
      if (x)
      {
        x += 5;
        double v = atof(x);
        if (v >= 0.0)
        {
          series_push(&rtts, v);
          if (df)
            fprintf(df, "%.3f\n", v);
        }
      }
    }

    if (strstr(line, "packet loss"))
    {
      char *pct = strstr(line, "%");
      if (pct)
      {
        char *x = pct;
        while (x > line && ((*(x - 1) >= '0' && *(x - 1) <= '9') || *(x - 1) == '.'))
          --x;
        *loss_pct = atof(x);
      }
    }

    if (strstr(line, "unknown host") || strstr(line, "Name or service not known"))
      (*errors)++;
  }

  int rc = pclose(p);
  if (rc != 0)
    (*errors)++;

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
  fprintf(f, "step,throughput_mb_s,iops,lat_avg_us,lat_p50_us,lat_p95_us,lat_p99_us,lat_p999_us,lat_max_us,outliers\n");
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
  fprintf(f, "step,loss_pct,p95_ms,p99_ms,min_ms,avg_ms,max_ms,errors\n");
  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind != WK_PING)
      continue;
    fprintf(f, "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu64 "\n",
            res[i].step->name,
            res[i].packet_loss_pct,
            res[i].ping_p95_ms,
            res[i].ping_p99_ms,
            res[i].ping_min_ms,
            res[i].ping_avg_ms,
            res[i].ping_max_ms,
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

static void write_run_status(const char *run_dir, const char *status, const char *stage, const char *message)
{
  char p[PATH_MAX];
  if (join_path(p, sizeof(p), run_dir, "run_status.json") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;
  fprintf(f, "{\n  \"status\": \"%s\",\n  \"stage\": \"%s\",\n  \"message\": \"%s\",\n  \"warnings\": %d,\n  \"errors\": %d\n}\n",
          status ? status : "unknown",
          stage ? stage : "unknown",
          message ? message : "",
          g_run_msgs.warning_count,
          g_run_msgs.error_count);
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

static void write_report_md(const char *run_dir, const Scenario *sc, const Collector *c, const StepResult *res, int nres, const RunMessages *msgs, const RunContext *run_ctx, double run_sec)
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
  fprintf(f, "- Run ID: `%s`\n", (run_ctx && run_ctx->run_id[0]) ? run_ctx->run_id : "n/a");
  if (run_ctx && run_ctx->created_at > 0)
    fprintf(f, "- Run date (epoch): %lld\n", (long long)run_ctx->created_at);
  fprintf(f, "- Duration (sec): %.3f\n", run_sec);
  fprintf(f, "- Samples: %zu\n", c->nrows);
  fprintf(f, "- Tests executed: %d of %d\n\n", nres, sc->step_count);

  fprintf(f, "## Test coverage\n\n");
  for (int i = 0; i < sc->step_count; ++i)
  {
    int done = 0;
    for (int j = 0; j < nres; ++j)
    {
      if (res[j].step == &sc->steps[i])
      {
        done = 1;
        break;
      }
    }
    fprintf(f, "- [%c] %s (%s)\n", done ? 'x' : ' ', sc->steps[i].name ? sc->steps[i].name : "step", done ? "executed" : "skipped");
  }
  fprintf(f, "\n");

  fprintf(f, "## Warnings\n\n");
  if (msgs && msgs->warning_count > 0)
  {
    for (int i = 0; i < msgs->warning_count; ++i)
      fprintf(f, "- %s\n", msgs->warnings[i]);
  }
  else
  {
    fprintf(f, "- none\n");
  }
  fprintf(f, "\n");

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
  if (msgs && msgs->warning_count > 0)
    fprintf(f, "- Обнаружены недоступные каналы телеметрии, см. раздел Warnings.\n");
  fclose(f);
}
/* ---------- benchmark orchestration ---------- */

static int run_benchmark(Scenario sc, double duration_scale, int replace_latest)
{
  double run_started_at = now_sec();
  g_stop = 0;
  memset(&g_run_msgs, 0, sizeof(g_run_msgs));
  detect_telemetry_channel_warnings();

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

  RunContext run_ctx;
  if (coordinator_prepare_run(&sc, replace_latest, &run_ctx) != 0)
  {
    fprintf(stderr, "failed to prepare run context\n");
    return 1;
  }

  if (mkdir_p(run_ctx.run_dir) != 0)
  {
    fprintf(stderr, "failed to create run directory: %s\n", run_ctx.run_dir);
    return 1;
  }

  write_run_status(run_ctx.run_dir, "created", "prepare", "run directory created");
  coordinator_write_run_id(&run_ctx);
  write_scenario_json(run_ctx.run_dir, &sc);
  write_system_info(run_ctx.run_dir);

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
  snprintf(c.out_dir, sizeof(c.out_dir), "%s", run_ctx.run_dir);
  pthread_mutex_init(&c.lock, NULL);

  pthread_t th;
  pthread_create(&th, NULL, collector_thread, &c);
  write_run_status(run_ctx.run_dir, "running", "prepare", "telemetry started");

  NoiseContext *noise = noise_start(sc.noise_mode, 1, run_ctx.run_dir);

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
    if (strstr(st->name, "warmup") != NULL)
      write_run_status(run_ctx.run_dir, "running", "warmup", st->name);
    else
      write_run_status(run_ctx.run_dir, "running", "main", st->name);

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
      r.throughput_mb_s = run_storage(st, run_ctx.run_dir,
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
               run_ctx.run_dir);
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

  write_run_status(run_ctx.run_dir, "running", "stop_modules", "stopping background modules");
  write_telemetry_csv(&c);
  write_cpu_csv(run_ctx.run_dir, results, nres);
  write_memory_csv(run_ctx.run_dir, results, nres);
  write_storage_csv(run_ctx.run_dir, results, nres);
  write_network_csv(run_ctx.run_dir, results, nres);
  write_jitter_csv(run_ctx.run_dir, results, nres);
  write_run_status(run_ctx.run_dir, "running", "metrics", "calculating metrics");
  write_metrics_json(run_ctx.run_dir, &sc, &c, results, nres);
  write_run_status(run_ctx.run_dir, "running", "report", "building report");
  write_report_md(run_ctx.run_dir, &sc, &c, results, nres, &g_run_msgs, &run_ctx, now_sec() - run_started_at);

  write_run_status(run_ctx.run_dir, g_stop ? "interrupted" : "completed", "done", g_stop ? "stopped" : "ok");

  free(c.rows);
  fprintf(stdout, "Done. Results: %s\n", run_ctx.run_dir);
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