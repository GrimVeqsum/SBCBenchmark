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

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_ROWS 864000
#define MAX_STEPS 16
#define MAX_CPUS 256

typedef enum
{
  WK_IDLE,
  WK_CPU_BURN,
  WK_STORAGE,
  WK_PING,
  WK_NN
} WorkKind;

typedef struct
{
  const char *name;
  WorkKind kind;
  int duration_sec;
  int threads;
  const char *arg;
} Step;

typedef struct
{
  const char *name;
  int sample_sec;
  double critical_temp_c;
  double target_ping_p99_ms;
  double ref_perf_per_watt;
  double assumed_power_w;
  Step steps[MAX_STEPS];
  int step_count;
} Scenario;

typedef struct
{
  double ts;
  double temp_c;
  double cpu_freq_mhz;
  double cpu_util_pct;
  double mem_used_pct;
  double power_w;
  double psi_cpu_some_avg10;
  double psi_io_some_avg10;
  double psi_mem_some_avg10;
} Row;

typedef struct
{
  const Step *step;
  double ops_per_sec;
  double throughput_mb_s;
  double ping_p99_ms;
  double packet_loss_pct;
  double nn_inf_per_sec;
} StepResult;

typedef struct
{
  Row *rows;
  size_t cap;
  size_t nrows;
  pthread_mutex_t lock;
  int sample_sec;
  volatile sig_atomic_t stop;
  char out_dir[PATH_MAX];
} Collector;

static volatile sig_atomic_t g_stop = 0;

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
  snprintf(path, sizeof(path), "%s/telemetry.csv", c->out_dir);
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
      usleep(100000);
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
    usleep(100000);
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
    usleep(100000);
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
  snprintf(path, sizeof(path), "%s/storage_test.bin", out_dir);
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0)
    return -1.0;

  size_t block = 4 * 1024 * 1024;
  if (s->arg && strcmp(s->arg, "1M") == 0)
    block = 1024 * 1024;
  if (s->arg && strcmp(s->arg, "8M") == 0)
    block = 8 * 1024 * 1024;
  char *buf = NULL;
  if (posix_memalign((void **)&buf, 4096, block) != 0 || !buf)
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
    fdatasync(fd);
  }
  double elapsed = now_sec() - t0;
  free(buf);
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

static Scenario scenario_from_name(const char *name)
{
  Scenario s;
  memset(&s, 0, sizeof(s));
  s.sample_sec = 1;
  s.critical_temp_c = 85.0;
  s.target_ping_p99_ms = 25.0;
  s.ref_perf_per_watt = 10000000.0;
  s.assumed_power_w = 5.0;

  if (strcmp(name, "server_edge") == 0)
  {
    s.name = "server_edge";
    s.critical_temp_c = 90.0;
    s.target_ping_p99_ms = 40.0;
    s.steps[0] = (Step){"warmup", WK_CPU_BURN, 180, 4, NULL};
    s.steps[1] = (Step){"network_latency", WK_PING, 120, 0, "8.8.8.8"};
    s.steps[2] = (Step){"storage_write", WK_STORAGE, 180, 0, "8M"};
    s.steps[3] = (Step){"cpu_steady", WK_CPU_BURN, 900, 4, NULL};
    s.step_count = 4;
  }
  else if (strcmp(name, "embedded") == 0)
  {
    s.name = "embedded";
    s.critical_temp_c = 85.0;
    s.target_ping_p99_ms = 30.0;
    s.steps[0] = (Step){"boot_settle", WK_IDLE, 60, 0, NULL};
    s.steps[1] = (Step){"control_compute", WK_CPU_BURN, 120, 2, NULL};
    s.steps[2] = (Step){"persistent_storage", WK_STORAGE, 90, 0, "1M"};
    s.steps[3] = (Step){"link_health", WK_PING, 60, 0, "1.1.1.1"};
    s.steps[4] = (Step){"steady_control", WK_CPU_BURN, 240, 2, NULL};
    s.step_count = 5;
  }
  else if (strcmp(name, "neural") == 0)
  {
    s.name = "neural";
    s.critical_temp_c = 90.0;
    s.target_ping_p99_ms = 35.0;
    s.ref_perf_per_watt = 15000000.0;
    s.steps[0] = (Step){"idle_baseline", WK_IDLE, 60, 0, NULL};
    s.steps[1] = (Step){"nn_warmup", WK_NN, 120, 2, "32"};
    s.steps[2] = (Step){"storage_checkpoint", WK_STORAGE, 60, 0, "4M"};
    s.steps[3] = (Step){"network_probe", WK_PING, 45, 0, "1.1.1.1"};
    s.steps[4] = (Step){"nn_steady", WK_NN, 300, 2, "48"};
    s.step_count = 5;
  }
  else if (strcmp(name, "iot_controller") == 0)
  {
    s.name = "iot_controller";
    s.critical_temp_c = 80.0;
    s.target_ping_p99_ms = 20.0;
    s.ref_perf_per_watt = 7000000.0;
    s.assumed_power_w = 3.0;
    s.steps[0] = (Step){"idle_baseline", WK_IDLE, 120, 0, NULL};
    s.steps[1] = (Step){"sensor_batch_compute", WK_CPU_BURN, 60, 1, NULL};
    s.steps[2] = (Step){"persist_batch", WK_STORAGE, 45, 0, "1M"};
    s.steps[3] = (Step){"uplink_health", WK_PING, 60, 0, "1.1.1.1"};
    s.steps[4] = (Step){"sleep_window", WK_IDLE, 180, 0, NULL};
    s.step_count = 5;
  }
  else
  {
    s.name = "baseline";
    s.steps[0] = (Step){"boot_settle", WK_IDLE, 30, 0, NULL};
    s.steps[1] = (Step){"cpu_warmup", WK_CPU_BURN, 120, 2, NULL};
    s.steps[2] = (Step){"storage_probe", WK_STORAGE, 90, 0, "4M"};
    s.steps[3] = (Step){"network_probe", WK_PING, 45, 0, "1.1.1.1"};
    s.steps[4] = (Step){"cpu_steady", WK_CPU_BURN, 300, 2, NULL};
    s.step_count = 5;
  }
  return s;
}

static int mkdir_p(const char *path)
{
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; ++p)
  {
    if (*p == '/')
    {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  return mkdir(tmp, 0755);
}

static void write_summary(const Scenario *sc, const Collector *c, const StepResult *res, int nres)
{
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/summary.json", c->out_dir);
  FILE *f = fopen(path, "w");
  if (!f)
    return;

  double temp_max = -1, temp_start = -1, temp_end = -1;
  double freq_sum = 0, util_sum = 0, power_sum = 0;
  int freq_n = 0, util_n = 0, power_n = 0;
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
    if (r->power_w > 0)
    {
      power_sum += r->power_w;
      power_n++;
    }
  }

  double cpu_ops[16];
  int cpu_n = 0;
  double ping_p99[16];
  int ping_n = 0;
  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind == WK_CPU_BURN && res[i].ops_per_sec > 0 && cpu_n < 16)
      cpu_ops[cpu_n++] = res[i].ops_per_sec;
    if (res[i].step->kind == WK_NN && res[i].nn_inf_per_sec > 0 && cpu_n < 16)
      cpu_ops[cpu_n++] = res[i].nn_inf_per_sec;
    if (res[i].step->kind == WK_PING && res[i].ping_p99_ms > 0 && ping_n < 16)
      ping_p99[ping_n++] = res[i].ping_p99_ms;
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

  fprintf(f, "{\n");
  fprintf(f, "  \"scenario\": \"%s\",\n", sc->name);
  fprintf(f, "  \"aggregates\": {\n");
  fprintf(f, "    \"temp_c_start\": %.3f,\n", temp_start);
  fprintf(f, "    \"temp_c_end\": %.3f,\n", temp_end);
  fprintf(f, "    \"temp_c_max\": %.3f,\n", temp_max);
  fprintf(f, "    \"cpu_freq_mhz_avg\": %.3f,\n", freq_n ? freq_sum / freq_n : -1.0);
  fprintf(f, "    \"cpu_util_pct_avg\": %.3f,\n", util_n ? util_sum / util_n : -1.0);
  fprintf(f, "    \"power_w_avg\": %.3f,\n", power_n ? power_sum / power_n : -1.0);
  fprintf(f, "    \"perf_per_watt\": %.3f\n", perf_w);
  fprintf(f, "  },\n");
  fprintf(f, "  \"score\": %.2f,\n", score);
  fprintf(f, "  \"steps\": [\n");
  for (int i = 0; i < nres; ++i)
  {
    fprintf(f, "    {\"name\":\"%s\",\"kind\":%d,\"ops_per_sec\":%.3f,\"nn_inf_per_sec\":%.3f,\"throughput_mb_s\":%.3f,\"ping_p99_ms\":%.3f,\"packet_loss_pct\":%.3f}%s\n",
            res[i].step->name, (int)res[i].step->kind, res[i].ops_per_sec,
            res[i].nn_inf_per_sec, res[i].throughput_mb_s, res[i].ping_p99_ms, res[i].packet_loss_pct,
            (i + 1 < nres) ? "," : "");
  }
  fprintf(f, "  ]\n}\n");
  fclose(f);
}

static void write_report_md(const Scenario *sc, const Collector *c, const StepResult *res, int nres)
{
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/report.md", c->out_dir);
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "# SBC Benchmark Report (C)\n\n");
  fprintf(f, "- Scenario: **%s**\n", sc->name);
  fprintf(f, "- Samples: **%zu**\n\n", c->nrows);
  fprintf(f, "## Steps\n");
  for (int i = 0; i < nres; ++i)
  {
    fprintf(f, "- `%s`: kind=%d, ops/s=%.3f, nn/s=%.3f, storage(MB/s)=%.3f, ping_p99(ms)=%.3f, loss(%%)=%.3f\n",
            res[i].step->name, (int)res[i].step->kind, res[i].ops_per_sec, res[i].nn_inf_per_sec,
            res[i].throughput_mb_s, res[i].ping_p99_ms, res[i].packet_loss_pct);
  }
  fclose(f);
}

int main(int argc, char **argv)
{
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);

  const char *scenario_name = (argc >= 2) ? argv[1] : "baseline";
  double duration_scale = 1.0;
  if (argc >= 3)
  {
    duration_scale = atof(argv[2]);
    if (duration_scale <= 0.0)
      duration_scale = 1.0;
  }
  Scenario sc = scenario_from_name(scenario_name);
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

  time_t tt = time(NULL);
  struct tm tm;
  localtime_r(&tt, &tm);
  char run_dir[PATH_MAX];
  snprintf(run_dir, sizeof(run_dir), "runs_v4_c/%s_%04d%02d%02d_%02d%02d%02d",
           sc.name, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  mkdir_p(run_dir);

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
        usleep(100000);
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
  snprintf(meta_path, sizeof(meta_path), "%s/meta.txt", run_dir);
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