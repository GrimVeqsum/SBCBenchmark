#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "sbc_bench_telemetry.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#endif

static double now_sec_local(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_100ms_local(void)
{
#ifdef _WIN32
  Sleep(100);
#else
  usleep(100000);
#endif
}

static int read_text_local(const char *path, char *buf, size_t n)
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

static double read_first_double_local(const char *path, double div, double defv)
{
  char b[256];
  if (read_text_local(path, b, sizeof(b)) != 0)
    return defv;
  char *end = NULL;
  double v = strtod(b, &end);
  if (end == b)
    return defv;
  return v / div;
}

static int join_path_local(char *dst, size_t dst_sz, const char *dir, const char *name)
{
  if (!dst || !dir || !name || dst_sz == 0)
    return -1;
  int n = snprintf(dst, dst_sz, "%s/%s", dir, name);
  if (n < 0 || (size_t)n >= dst_sz)
    return -1;
  return 0;
}

static void add_warning_local(RunMessages *msgs, const char *msg)
{
  if (!msgs || !msg || !*msg)
    return;
  if (msgs->warning_count >= MAX_WARNINGS)
    return;
  snprintf(msgs->warnings[msgs->warning_count], sizeof(msgs->warnings[msgs->warning_count]), "%s", msg);
  msgs->warning_count++;
}

static int list_cpu_freq_paths_local(char paths[MAX_CPUS][PATH_MAX])
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

static double read_max_temp_c_local(void)
{
  double mx = -1.0;
  for (int i = 0; i < 128; ++i)
  {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "/sys/class/thermal/thermal_zone%d/temp", i);
    if (access(p, R_OK) != 0)
      continue;
    double t = read_first_double_local(p, 1000.0, -1.0);
    if (t > mx)
      mx = t;
  }
  return mx;
}

static double read_avg_cpu_freq_mhz_local(void)
{
  static char paths[MAX_CPUS][PATH_MAX];
  static int npaths = -1;
  if (npaths < 0)
    npaths = list_cpu_freq_paths_local(paths);
  if (npaths <= 0)
    return -1.0;

  double sum = 0.0;
  int n = 0;
  for (int i = 0; i < npaths; ++i)
  {
    double v = read_first_double_local(paths[i], 1000.0, -1.0);
    if (v > 0.0)
    {
      sum += v;
      n++;
    }
  }
  return n ? sum / n : -1.0;
}

static double read_cpu_util_pct_local(void)
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

static double read_mem_used_pct_local(void)
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

static double read_power_w_local(void)
{
  char p[PATH_MAX];
  for (int i = 0; i < 256; ++i)
  {
    snprintf(p, sizeof(p), "/sys/class/hwmon/hwmon%d/power1_input", i);
    if (access(p, R_OK) == 0)
      return read_first_double_local(p, 1000000.0, -1.0);
  }
  for (int i = 0; i < 16; ++i)
  {
    snprintf(p, sizeof(p), "/sys/class/power_supply/BAT%d/power_now", i);
    if (access(p, R_OK) == 0)
      return read_first_double_local(p, 1000000.0, -1.0);
  }
  return -1.0;
}

static double read_psi_avg10_local(const char *kind)
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

void telemetry_detect_channel_warnings(RunMessages *msgs)
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
    add_warning_local(msgs, "telemetry: thermal zones are unavailable");

  char paths[MAX_CPUS][PATH_MAX];
  if (list_cpu_freq_paths_local(paths) <= 0)
    add_warning_local(msgs, "telemetry: cpu frequency channels are unavailable");

  if (access("/proc/pressure/cpu", R_OK) != 0)
    add_warning_local(msgs, "telemetry: PSI cpu is unavailable");
  if (access("/proc/pressure/io", R_OK) != 0)
    add_warning_local(msgs, "telemetry: PSI io is unavailable");
  if (access("/proc/pressure/memory", R_OK) != 0)
    add_warning_local(msgs, "telemetry: PSI memory is unavailable");
}

void telemetry_collector_init(Collector *c, Row *rows, size_t cap, int sample_sec, const char *out_dir)
{
  memset(c, 0, sizeof(*c));
  c->rows = rows;
  c->cap = cap;
  c->sample_sec = sample_sec > 0 ? sample_sec : 1;
  if (out_dir)
    snprintf(c->out_dir, sizeof(c->out_dir), "%s", out_dir);
  pthread_mutex_init(&c->lock, NULL);
}

void *telemetry_collector_thread(void *arg)
{
  Collector *c = (Collector *)arg;
  while (!c->stop)
  {
    Row r;
    r.ts = now_sec_local();
    r.temp_c = read_max_temp_c_local();
    r.cpu_freq_mhz = read_avg_cpu_freq_mhz_local();
    r.cpu_util_pct = read_cpu_util_pct_local();
    r.mem_used_pct = read_mem_used_pct_local();
    r.power_w = read_power_w_local();
    r.psi_cpu_some_avg10 = read_psi_avg10_local("cpu");
    r.psi_io_some_avg10 = read_psi_avg10_local("io");
    r.psi_mem_some_avg10 = read_psi_avg10_local("memory");

    pthread_mutex_lock(&c->lock);
    if (c->nrows < c->cap)
      c->rows[c->nrows++] = r;
    pthread_mutex_unlock(&c->lock);

    for (int i = 0; i < c->sample_sec * 10 && !c->stop; ++i)
      sleep_100ms_local();
  }
  return NULL;
}

void telemetry_write_csv(const Collector *c)
{
  char path[PATH_MAX];
  if (join_path_local(path, sizeof(path), c->out_dir, "telemetry.csv") != 0)
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