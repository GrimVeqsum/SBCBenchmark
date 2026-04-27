#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <locale.h>
#include <dirent.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <malloc.h>
#endif

#include "sbc_bench_types.h"
#include "sbc_bench_scenarios.h"

static volatile sig_atomic_t g_stop = 0;

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
static void free_aligned_portable(void *p)
{
  _aligned_free(p);
}
static int fdatasync_portable(int fd)
{
  return _commit(fd);
}
static int mkdir_portable(const char *path, mode_t mode)
{
  (void)mode;
  return _mkdir(path);
}
static void sleep_100ms(void)
{
  Sleep(100);
}
static void localtime_r_portable(const time_t *tt, struct tm *out)
{
  localtime_s(out, tt);
}
#else
static int posix_memalign_portable(void **memptr, size_t alignment, size_t size)
{
  return posix_memalign(memptr, alignment, size);
}
static void free_aligned_portable(void *p)
{
  free(p);
}
static int fdatasync_portable(int fd)
{
  return fdatasync(fd);
}
static int mkdir_portable(const char *path, mode_t mode)
{
  return mkdir(path, mode);
}
static void sleep_100ms(void)
{
  usleep(100000);
}
static void localtime_r_portable(const time_t *tt, struct tm *out)
{
  localtime_r(tt, out);
}
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
  {
    buf[--len] = '\0';
  }
  return 0;
}

static double read_first_double(const char *path, double div, double def)
{
  char b[256];
  if (read_text(path, b, sizeof(b)) != 0)
    return def;
  char *end = NULL;
  double v = strtod(b, &end);
  if (end == b)
    return def;
  return v / div;
}

static int list_cpu_freq_paths(char paths[MAX_CPUS][PATH_MAX])
{
  int n = 0;
  for (int i = 0; i < MAX_CPUS; ++i)
  {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
    if (access(p, R_OK) == 0)
    {
      strncpy(paths[n++], p, PATH_MAX - 1);
    }
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
    if (v > 0)
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

static int join_path(char *dst, size_t dst_sz, const char *dir, const char *name)
{
  if (!dst || !dir || !name || dst_sz == 0)
    return -1;
  int n = snprintf(dst, dst_sz, "%s/%s", dir, name);
  if (n < 0 || (size_t)n >= dst_sz)
  {
    fprintf(stderr, "path too long: %s/%s\n", dir, name);
    return -1;
  }
  return 0;
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

static void write_csv(const Collector *c)
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
    fprintf(f, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            r->ts, r->temp_c, r->cpu_freq_mhz, r->cpu_util_pct, r->mem_used_pct, r->power_w,
            r->psi_cpu_some_avg10, r->psi_io_some_avg10, r->psi_mem_some_avg10);
  }
  fclose(f);
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

typedef struct
{
  volatile sig_atomic_t *stop;
  uint64_t *counter;
} BurnArgs;

static void *burn_worker(void *arg)
{
  BurnArgs *a = (BurnArgs *)arg;
  uint64_t c = 0;
  uint32_t x = 1;
  while (!*a->stop && !g_stop)
  {
    for (int i = 0; i < 20000; ++i)
    {
      x = x * 1664525u + 1013904223u;
      c++;
    }
    *a->counter = c;
  }
  return NULL;
}

static int cmp_double(const void *a, const void *b)
{
  double da = *(const double *)a;
  double db = *(const double *)b;
  return (da > db) - (da < db);
}

static double percentile(double *arr, size_t n, double p)
{
  if (n == 0)
    return -1.0;
  qsort(arr, n, sizeof(double), cmp_double);
  double pos = ((double)(n - 1)) * p / 100.0;
  size_t lo = (size_t)floor(pos), hi = (size_t)ceil(pos);
  if (lo == hi)
    return arr[lo];
  double w = pos - (double)lo;
  return arr[lo] * (1.0 - w) + arr[hi] * w;
}

static double run_cpu_burn(const Step *s)
{
  int n = s->threads > 0 ? s->threads : 1;
  if (n > MAX_CPUS)
    n = MAX_CPUS;
  pthread_t th[MAX_CPUS];
  uint64_t counters[MAX_CPUS];
  BurnArgs args[MAX_CPUS];
  volatile sig_atomic_t stop = 0;
  memset(counters, 0, sizeof(counters));

  double t0 = now_sec();
  for (int i = 0; i < n; ++i)
  {
    args[i].stop = &stop;
    args[i].counter = &counters[i];
    pthread_create(&th[i], NULL, burn_worker, &args[i]);
  }
  for (int i = 0; i < s->duration_sec * 10 && !g_stop; ++i)
    sleep_100ms();
  stop = 1;
  for (int i = 0; i < n; ++i)
    pthread_join(th[i], NULL);
  double elapsed = now_sec() - t0;
  uint64_t total = 0;
  for (int i = 0; i < n; ++i)
    total += counters[i];
  return elapsed > 0 ? (double)total / elapsed : 0.0;
}

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
  while (!*a->stop && !g_stop)
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
    *a->counter = c;
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
  uint64_t counters[MAX_CPUS];
  NnArgs args[MAX_CPUS];
  volatile sig_atomic_t stop = 0;
  memset(counters, 0, sizeof(counters));

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

static double run_storage(const Step *s, const char *out_dir)
{
  char path[PATH_MAX];
  if (join_path(path, sizeof(path), out_dir, "storage_test.bin") != 0)
    return -1.0;
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
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

  uint64_t written = 0;
  double t0 = now_sec();
  while (!g_stop && now_sec() - t0 < s->duration_sec)
  {
    ssize_t wr = write(fd, buf, block);
    if (wr < 0)
    {
      if (errno == EINTR)
        continue;
      break;
    }
    written += (uint64_t)wr;
    fdatasync_portable(fd);
  }
  double elapsed = now_sec() - t0;
  free_aligned_portable(buf);
  close(fd);
  unlink(path);
  if (elapsed <= 0.0)
    return -1.0;
  return ((double)written / (1024.0 * 1024.0)) / elapsed;
}

static void parse_ping_stats(const char *line, double *loss_pct, double *p99)
{
  if (strstr(line, "% packet loss"))
  {
    const char *p = strstr(line, "% packet loss");
    if (p)
    {
      const char *s = p;
      while (s > line && (*(s - 1) == '.' || (*(s - 1) >= '0' && *(s - 1) <= '9')))
        s--;
      *loss_pct = atof(s);
    }
  }
  if (strstr(line, "min/avg/max"))
  {
    double minv = 0, avgv = 0, maxv = 0;
    char *eq = strchr((char *)line, '=');
    if (eq && sscanf(eq + 1, " %lf/%lf/%lf", &minv, &avgv, &maxv) == 3)
    {
      *p99 = maxv; // without full histogram use max as conservative tail estimate
    }
  }
}

static void run_ping(const Step *s, double *loss_pct, double *p99)
{
  *loss_pct = -1.0;
  *p99 = -1.0;
  char cmd[512];
  const char *host = s->arg ? s->arg : "1.1.1.1";
  snprintf(cmd, sizeof(cmd), "ping -i 0.2 -w %d %s 2>&1", s->duration_sec, host);
  FILE *p = popen(cmd, "r");
  if (!p)
    return;
  char line[1024];
  while (fgets(line, sizeof(line), p))
    parse_ping_stats(line, loss_pct, p99);
  pclose(p);
}

static const char *score_band(double score)
{
  if (score >= 80.0)
    return "high_fit";
  if (score >= 60.0)
    return "medium_fit";
  return "low_fit";
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

static void write_summary(const Scenario *sc, const Collector *c, const StepResult *res, int nres)
{
  char path[PATH_MAX];
  if (join_path(path, sizeof(path), c->out_dir, "summary.json") != 0)
    return;
  FILE *f = fopen(path, "w");
  if (!f)
    return;

  double temp_max = -1, temp_start = -1, temp_end = -1;
  double freq_sum = 0, util_sum = 0, power_sum = 0, mem_sum = 0;
  double psi_cpu_max = -1.0, psi_io_max = -1.0, psi_mem_max = -1.0;
  int freq_n = 0, util_n = 0, power_n = 0, mem_n = 0;
  if (c->nrows > 0)
  {
    temp_start = c->rows[0].temp_c;
    temp_end = c->rows[c->nrows - 1].temp_c;
  }
  for (size_t i = 0; i < c->nrows; ++i)
  {
    const Row *r = &c->rows[i];
    if (r->temp_c > temp_max)
      temp_max = r->temp_c;
    if (r->cpu_freq_mhz > 0)
    {
      freq_sum += r->cpu_freq_mhz;
      freq_n++;
    }
    if (r->cpu_util_pct >= 0)
    {
      util_sum += r->cpu_util_pct;
      util_n++;
    }
    if (r->mem_used_pct >= 0)
    {
      mem_sum += r->mem_used_pct;
      mem_n++;
    }
    if (r->power_w > 0)
    {
      power_sum += r->power_w;
      power_n++;
    }
    if (r->psi_cpu_some_avg10 > psi_cpu_max)
      psi_cpu_max = r->psi_cpu_some_avg10;
    if (r->psi_io_some_avg10 > psi_io_max)
      psi_io_max = r->psi_io_some_avg10;
    if (r->psi_mem_some_avg10 > psi_mem_max)
      psi_mem_max = r->psi_mem_some_avg10;
  }

  double cpu_ops[16];
  int cpu_n = 0;
  double ping_p99[16];
  int ping_n = 0;
  double storage_sum = 0.0, nn_sum = 0.0, cpu_sum = 0.0, loss_sum = 0.0;
  int storage_n = 0, nn_n = 0, cpu_step_n = 0, loss_n = 0;
  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind == WK_CPU_BURN && res[i].ops_per_sec > 0)
    {
      if (cpu_n < 16)
        cpu_ops[cpu_n++] = res[i].ops_per_sec;
      cpu_sum += res[i].ops_per_sec;
      cpu_step_n++;
    }
    if (res[i].step->kind == WK_NN && res[i].nn_inf_per_sec > 0)
    {
      if (cpu_n < 16)
        cpu_ops[cpu_n++] = res[i].nn_inf_per_sec;
      nn_sum += res[i].nn_inf_per_sec;
      nn_n++;
    }
    if (res[i].step->kind == WK_PING && res[i].ping_p99_ms > 0 && ping_n < 16)
      ping_p99[ping_n++] = res[i].ping_p99_ms;
    if (res[i].step->kind == WK_PING && res[i].packet_loss_pct >= 0.0)
    {
      loss_sum += res[i].packet_loss_pct;
      loss_n++;
    }
    if (res[i].step->kind == WK_STORAGE && res[i].throughput_mb_s > 0)
    {
      storage_sum += res[i].throughput_mb_s;
      storage_n++;
    }
  }

  double perf_w = 0.0;
  if (cpu_n > 0)
  {
    double cpu_avg = 0;
    for (int i = 0; i < cpu_n; ++i)
      cpu_avg += cpu_ops[i];
    cpu_avg /= cpu_n;
    double pavg = power_n ? (power_sum / power_n) : sc->assumed_power_w;
    perf_w = cpu_avg / (pavg > 0.001 ? pavg : 1.0);
  }

  double stability = 0.0;
  if (cpu_n >= 2)
  {
    double first = cpu_ops[0];
    double second = 0.0;
    for (int i = cpu_n / 2; i < cpu_n; ++i)
      second += cpu_ops[i];
    second /= (cpu_n - cpu_n / 2);
    double drift = (first > 0) ? fmax(0.0, (first - second) / first) : 1.0;
    stability = fmax(0.0, 1.0 - drift);
  }

  double thermal = 0.0;
  if (temp_max > 0)
    thermal = fmax(0.0, fmin(1.0, (sc->critical_temp_c - temp_max) / sc->critical_temp_c));
  double tail = 0.5;
  if (ping_n > 0)
  {
    double p99 = 0;
    for (int i = 0; i < ping_n; ++i)
      p99 += ping_p99[i];
    p99 /= ping_n;
    tail = fmax(0.0, fmin(1.0, sc->target_ping_p99_ms / fmax(sc->target_ping_p99_ms, p99)));
  }
  double perf_n = fmax(0.0, fmin(1.0, perf_w / sc->ref_perf_per_watt));
  double score = 100.0 * (0.35 * perf_n + 0.25 * thermal + 0.25 * stability + 0.15 * tail);
  const char *band = score_band(score);
  double cpu_avg = cpu_step_n ? cpu_sum / cpu_step_n : -1.0;
  double nn_avg = nn_n ? nn_sum / nn_n : -1.0;
  double storage_avg = storage_n ? storage_sum / storage_n : -1.0;
  double ping_avg = -1.0;
  if (ping_n > 0)
  {
    double sum = 0.0;
    for (int i = 0; i < ping_n; ++i)
      sum += ping_p99[i];
    ping_avg = sum / ping_n;
  }
  double loss_avg = loss_n ? loss_sum / loss_n : -1.0;

  fprintf(f, "{\n");
  fprintf(f, "  \"scenario\": \"%s\",\n", sc->name);
  fprintf(f, "  \"description\": \"%s\",\n", sc->description ? sc->description : "");
  fprintf(f, "  \"metric_profile\": {\n");
  fprintf(f, "    \"primary_metrics\": [");
  for (int i = 0; i < sc->primary_metric_count; ++i)
    fprintf(f, "\"%s\"%s", sc->primary_metrics[i], (i + 1 < sc->primary_metric_count) ? ", " : "");
  fprintf(f, "],\n");
  fprintf(f, "    \"score_components\": {\"perf\": 0.35, \"thermal\": 0.25, \"stability\": 0.25, \"tail_latency\": 0.15},\n");
  fprintf(f, "    \"metric_units\": {\n");
  fprintf(f, "      \"cpu_ops_avg\": \"ops/sec\",\n");
  fprintf(f, "      \"nn_inf_per_sec_avg\": \"inference/sec\",\n");
  fprintf(f, "      \"storage_mb_s_avg\": \"MB/sec\",\n");
  fprintf(f, "      \"ping_p99_ms_avg\": \"ms\",\n");
  fprintf(f, "      \"packet_loss_pct_avg\": \"%%\",\n");
  fprintf(f, "      \"temp_c_max\": \"C\",\n");
  fprintf(f, "      \"power_w_avg\": \"W\"\n");
  fprintf(f, "    }\n");
  fprintf(f, "  },\n");
  fprintf(f, "  \"aggregates\": {\n");
  fprintf(f, "    \"temp_c_start\": %.3f,\n", temp_start);
  fprintf(f, "    \"temp_c_end\": %.3f,\n", temp_end);
  fprintf(f, "    \"temp_c_max\": %.3f,\n", temp_max);
  fprintf(f, "    \"cpu_freq_mhz_avg\": %.3f,\n", freq_n ? freq_sum / freq_n : -1.0);
  fprintf(f, "    \"cpu_util_pct_avg\": %.3f,\n", util_n ? util_sum / util_n : -1.0);
  fprintf(f, "    \"mem_used_pct_avg\": %.3f,\n", mem_n ? mem_sum / mem_n : -1.0);
  fprintf(f, "    \"power_w_avg\": %.3f,\n", power_n ? power_sum / power_n : -1.0);
  fprintf(f, "    \"psi_cpu_some_avg10_max\": %.3f,\n", psi_cpu_max);
  fprintf(f, "    \"psi_io_some_avg10_max\": %.3f,\n", psi_io_max);
  fprintf(f, "    \"psi_mem_some_avg10_max\": %.3f,\n", psi_mem_max);
  fprintf(f, "    \"perf_per_watt\": %.3f\n", perf_w);
  fprintf(f, "  },\n");
  fprintf(f, "  \"scenario_metrics\": {\n");
  fprintf(f, "    \"cpu_ops_avg\": %.3f,\n", cpu_avg);
  fprintf(f, "    \"nn_inf_per_sec_avg\": %.3f,\n", nn_avg);
  fprintf(f, "    \"storage_mb_s_avg\": %.3f,\n", storage_avg);
  fprintf(f, "    \"ping_p99_ms_avg\": %.3f,\n", ping_avg);
  fprintf(f, "    \"packet_loss_pct_avg\": %.3f,\n", loss_avg);
  fprintf(f, "    \"thermal_headroom_pct\": %.3f,\n", thermal * 100.0);
  fprintf(f, "    \"stability_score_pct\": %.3f,\n", stability * 100.0);
  fprintf(f, "    \"network_tail_score_pct\": %.3f,\n", tail * 100.0);
  fprintf(f, "    \"energy_efficiency_score_pct\": %.3f\n", perf_n * 100.0);
  fprintf(f, "  },\n");
  fprintf(f, "  \"score\": %.2f,\n", score);
  fprintf(f, "  \"fit_band\": \"%s\",\n", band);
  fprintf(f, "  \"steps\": [\n");
  for (int i = 0; i < nres; ++i)
  {
    fprintf(f, "    {\"name\":\"%s\",\"kind\":%d,\"kind_name\":\"%s\",\"load_profile\":\"%s\",\"purpose\":\"%s\",\"ops_per_sec\":%.3f,\"nn_inf_per_sec\":%.3f,\"throughput_mb_s\":%.3f,\"ping_p99_ms\":%.3f,\"packet_loss_pct\":%.3f}%s\n",
            res[i].step->name, (int)res[i].step->kind, kind_name(res[i].step->kind),
            res[i].step->load_profile ? res[i].step->load_profile : "",
            res[i].step->purpose ? res[i].step->purpose : "",
            res[i].ops_per_sec, res[i].nn_inf_per_sec, res[i].throughput_mb_s, res[i].ping_p99_ms, res[i].packet_loss_pct,
            (i + 1 < nres) ? "," : "");
  }
  fprintf(f, "  ]\n}\n");
  fclose(f);
}

static void write_report_md(const Scenario *sc, const Collector *c, const StepResult *res, int nres)
{
  char path[PATH_MAX];
  if (join_path(path, sizeof(path), c->out_dir, "report.md") != 0)
    return;
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "# SBC Benchmark Report (C)\n\n");
  fprintf(f, "- Scenario: **%s**\n", sc->name);
  fprintf(f, "- Samples: **%zu**\n\n", c->nrows);
  double temp_max = -1.0, power_sum = 0.0;
  int power_n = 0;
  for (size_t i = 0; i < c->nrows; ++i)
  {
    if (c->rows[i].temp_c > temp_max)
      temp_max = c->rows[i].temp_c;
    if (c->rows[i].power_w > 0.0)
    {
      power_sum += c->rows[i].power_w;
      power_n++;
    }
  }
  fprintf(f, "## Aggregates\n\n");
  fprintf(f, "- Max temperature: %.3f C\n", temp_max);
  if (power_n > 0)
    fprintf(f, "- Avg power: %.3f W\n", power_sum / power_n);
  else
    fprintf(f, "- Avg power: N/A\n");
  fprintf(f, "\n## Steps\n\n");
  fprintf(f, "| Step | Kind | Load profile | CPU ops/s | NN inf/s | Storage MB/s | Ping p99 ms | Loss %% |\n");
  fprintf(f, "|---|---|---|---:|---:|---:|---:|---:|\n");
  for (int i = 0; i < nres; ++i)
  {
    fprintf(f, "| %s | %s | %s | %.3f | %.3f | %.3f | %.3f | %.3f |\n",
            res[i].step->name, kind_name(res[i].step->kind), res[i].step->load_profile ? res[i].step->load_profile : "n/a", res[i].ops_per_sec, res[i].nn_inf_per_sec,
            res[i].throughput_mb_s, res[i].ping_p99_ms, res[i].packet_loss_pct);
  }
  fprintf(f, "\n## Metric glossary\n\n");
  fprintf(f, "- CPU ops/s: synthetic CPU throughput (ops/sec, больше — лучше).\n");
  fprintf(f, "- NN inf/s: скорость инференса (inference/sec, больше — лучше).\n");
  fprintf(f, "- Storage MB/s: скорость записи (MB/sec, больше — лучше).\n");
  fprintf(f, "- Ping p99 ms: хвостовая задержка сети (ms, меньше — лучше).\n");
  fprintf(f, "- Loss %%: потери пакетов (%%, меньше — лучше).\n");
  fclose(f);
}

static int run_benchmark(Scenario sc, double duration_scale, int replace_latest)
{
  g_stop = 0;
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
  {
    snprintf(run_dir, sizeof(run_dir), "runs_v4_c/%s_latest", sc.name);
  }
  else
  {
    snprintf(run_dir, sizeof(run_dir), "runs_v4_c/%s_%04d%02d%02d_%02d%02d%02d",
             sc.name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  }
  if (mkdir_p(run_dir) != 0)
  {
    fprintf(stderr, "failed to create run directory: %s\n", run_dir);
    return 1;
  }

  Collector c;
  memset(&c, 0, sizeof(c));
  c.cap = MAX_ROWS;
  c.rows = calloc(c.cap, sizeof(Row));
  if (!c.rows)
  {
    fprintf(stderr, "failed to allocate telemetry buffer\\n");
    return 1;
  }
  c.sample_sec = sc.sample_sec;
  snprintf(c.out_dir, sizeof(c.out_dir), "%s", run_dir);
  pthread_mutex_init(&c.lock, NULL);

  pthread_t th;
  pthread_create(&th, NULL, collector_thread, &c);

  StepResult results[MAX_STEPS];
  int nres = 0;

  for (int i = 0; i < sc.step_count && !g_stop; ++i)
  {
    const Step *st = &sc.steps[i];
    StepResult r;
    memset(&r, 0, sizeof(r));
    r.step = st;
    fprintf(stderr, "[STEP] %s (%d sec)\n", st->name, st->duration_sec);
    if (st->kind == WK_IDLE)
    {
      for (int k = 0; k < st->duration_sec * 10 && !g_stop; ++k)
        sleep_100ms();
    }
    else if (st->kind == WK_CPU_BURN)
    {
      r.ops_per_sec = run_cpu_burn(st);
    }
    else if (st->kind == WK_STORAGE)
    {
      r.throughput_mb_s = run_storage(st, run_dir);
    }
    else if (st->kind == WK_PING)
    {
      run_ping(st, &r.packet_loss_pct, &r.ping_p99_ms);
    }
    else if (st->kind == WK_NN)
    {
      r.nn_inf_per_sec = run_nn_inference(st);
    }
    results[nres++] = r;
  }

  c.stop = 1;
  pthread_join(th, NULL);
  write_csv(&c);
  write_summary(&sc, &c, results, nres);
  write_report_md(&sc, &c, results, nres);

  char meta_path[PATH_MAX];
  if (join_path(meta_path, sizeof(meta_path), run_dir, "meta.txt") != 0)
  {
    free(c.rows);
    return 1;
  }
  FILE *mf = fopen(meta_path, "w");
  if (mf)
  {
    fprintf(mf, "scenario=%s\n", sc.name);
    fprintf(mf, "sample_sec=%d\n", sc.sample_sec);
    fprintf(mf, "critical_temp_c=%.1f\n", sc.critical_temp_c);
    fclose(mf);
  }
  free(c.rows);

  fprintf(stderr, "Done. Results: %s\n", run_dir);
  return 0;
}

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
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "runs_v4_c/%s", ent->d_name);
    char summary[PATH_MAX];
    if (join_path(summary, sizeof(summary), path, "summary.json") != 0)
      continue;
    struct stat st;
    if (stat(summary, &st) != 0)
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
  char summary_path[PATH_MAX];
  if (join_path(summary_path, sizeof(summary_path), latest, "summary.json") != 0)
    return;
  fprintf(stdout, "\n=== Latest run: %s ===\n", latest);
  print_file_text(summary_path);
  fprintf(stdout, "\n");
}

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
        const char *all_scenarios[] = {"baseline", "iot", "embedded", "neural_host", "server_gateway"};
        int all_n = (int)(sizeof(all_scenarios) / sizeof(all_scenarios[0]));
        int rc = 0;
        for (int i = 0; i < all_n; ++i)
        {
          Scenario sci = scenario_from_name(all_scenarios[i]);
          rc = run_benchmark(sci, scale, replace_latest);
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