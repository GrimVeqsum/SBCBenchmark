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
#include "sbc_bench_report.h"
#include "sbc_bench_noise.h"
#include "sbc_bench_workloads.h"
#include "sbc_bench_storage.h"
#include "sbc_bench_network.h"
#include "sbc_bench_coordinator.h"

static volatile sig_atomic_t g_stop = 0;
static RunMessages g_run_msgs;
typedef enum
{
  RUN_PREPARE = 0,
  RUN_WARMUP = 1,
  RUN_MAIN = 2,
  RUN_STOP = 3,
  RUN_METRICS = 4,
  RUN_REPORT = 5,
  RUN_DONE = 6
} RunStage;

static void add_warning(const char *msg)
{
  if (!msg || !*msg)
    return;
  if (g_run_msgs.warning_count >= MAX_WARNINGS)
    return;
  snprintf(g_run_msgs.warnings[g_run_msgs.warning_count], sizeof(g_run_msgs.warnings[g_run_msgs.warning_count]), "%s", msg);
  g_run_msgs.warning_count++;
}

static const char *run_stage_name(RunStage st)
{
  switch (st)
  {
  case RUN_PREPARE:
    return "prepare";
  case RUN_WARMUP:
    return "warmup";
  case RUN_MAIN:
    return "main";
  case RUN_STOP:
    return "stop_modules";
  case RUN_METRICS:
    return "metrics";
  case RUN_REPORT:
    return "report";
  case RUN_DONE:
    return "done";
  default:
    return "unknown";
  }
}

static void update_run_status(const char *run_dir, const char *status, RunStage st, const char *message)
{
  report_write_run_status(run_dir, status, run_stage_name(st), message, &g_run_msgs);
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
static int mkdir_portable(const char *path, mode_t mode)
{
  (void)mode;
  return _mkdir(path);
}
static void sleep_100ms(void) { Sleep(100); }
static void localtime_r_portable(const time_t *tt, struct tm *out) { localtime_s(out, tt); }
#else
static int mkdir_portable(const char *path, mode_t mode) { return mkdir(path, mode); }
static void sleep_100ms(void) { usleep(100000); }
static void localtime_r_portable(const time_t *tt, struct tm *out) { localtime_r(tt, out); }
#endif

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
    add_warning("telemetry: thermal zones are unavailable (missing or no permissions)");

  char paths[MAX_CPUS][PATH_MAX];
  if (list_cpu_freq_paths(paths) <= 0)
    add_warning("telemetry: cpu frequency channels are unavailable (missing or no permissions)");

  if (access("/proc/pressure/cpu", R_OK) != 0)
    add_warning("telemetry: PSI cpu is unavailable (missing or no permissions)");
  if (access("/proc/pressure/io", R_OK) != 0)
    add_warning("telemetry: PSI io is unavailable (missing or no permissions)");
  if (access("/proc/pressure/memory", R_OK) != 0)
    add_warning("telemetry: PSI memory is unavailable (missing or no permissions)");
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
  {
    fprintf(stdout, "[INFO] Длительный тест: %d sec (~%.1f min)\n", total_duration, total_duration / 60.0);
    fprintf(stdout, "[INFO] Продолжить? [y/N]: ");
    char line[32] = {0};
    if (!fgets(line, sizeof(line), stdin) || (line[0] != 'y' && line[0] != 'Y'))
    {
      fprintf(stdout, "[INFO] Запуск отменён пользователем.\n");
      return 0;
    }
  }

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

  update_run_status(run_ctx.run_dir, "created", RUN_PREPARE, "run directory created");
  coordinator_write_run_id(&run_ctx);
  report_write_scenario_json(run_ctx.run_dir, &sc);
  report_write_system_info(run_ctx.run_dir);

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
  update_run_status(run_ctx.run_dir, "running", RUN_PREPARE, "telemetry started");

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
      update_run_status(run_ctx.run_dir, "running", RUN_WARMUP, st->name);
    else
      update_run_status(run_ctx.run_dir, "running", RUN_MAIN, st->name);

    if (st->kind == WK_IDLE)
    {
      for (int k = 0; k < st->duration_sec * 10 && !g_stop; ++k)
        sleep_100ms();
    }
    else if (st->kind == WK_CPU_BURN)
    {
      workload_run_cpu_burn(st, &g_stop, &r.ops_per_sec, &r.ops_window_start, &r.ops_window_end, &r.cpu_degradation_pct);
    }
    else if (st->kind == WK_STORAGE)
    {
      r.throughput_mb_s = storage_run(st, run_ctx.run_dir,
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
      network_run_ping(st,
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
      r.nn_inf_per_sec = workload_run_nn_inference(st, &g_stop);
    }
    else if (st->kind == WK_MEMORY)
    {
      {
        int was_clamped = 0;
        workload_run_memory_test(st, &r.mem_read_mb_s, &r.mem_write_mb_s, &r.mem_copy_mb_s, &g_stop, &was_clamped);
        if (was_clamped)
          add_warning("memory: buffer size was clamped to safe range [1MiB..1GiB]");
      }
    }
    else if (st->kind == WK_JITTER)
    {
      workload_run_jitter_test(st,
                               &r.jitter_avg_us,
                               &r.jitter_p50_us,
                               &r.jitter_p95_us,
                               &r.jitter_p99_us,
                               &r.jitter_max_us,
                               &r.jitter_over_500us,
                               &r.jitter_over_1000us,
                               &g_stop);
    }

    results[nres++] = r;
  }

  if (noise)
    noise_stop(noise);

  c.stop = 1;
  pthread_join(th, NULL);

  update_run_status(run_ctx.run_dir, "running", RUN_STOP, "stopping background modules");
  write_telemetry_csv(&c);
  report_write_step_csvs(run_ctx.run_dir, results, nres);
  update_run_status(run_ctx.run_dir, "running", RUN_METRICS, "calculating metrics");
  report_write_metrics_json(run_ctx.run_dir, &sc, &c, results, nres);
  update_run_status(run_ctx.run_dir, "running", RUN_REPORT, "building report");
  report_write_report_md(run_ctx.run_dir, &sc, &c, results, nres, &g_run_msgs, &run_ctx, now_sec() - run_started_at);

  update_run_status(run_ctx.run_dir, g_stop ? "interrupted" : "completed", RUN_DONE, g_stop ? "stopped" : "ok");

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