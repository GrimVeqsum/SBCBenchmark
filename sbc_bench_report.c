#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "sbc_bench_report.h"
#include "sbc_bench_metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct
{
  double *v;
  size_t n;
  size_t cap;
} SeriesLocal;

static void s_init(SeriesLocal *s, size_t cap)
{
  s->n = 0;
  s->cap = cap;
  s->v = (double *)calloc(cap ? cap : 1, sizeof(double));
}
static void s_push(SeriesLocal *s, double x)
{
  if (!s || !s->v || s->n >= s->cap)
    return;
  s->v[s->n++] = x;
}
static void s_free(SeriesLocal *s)
{
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}

static const char *kind_name_local(WorkKind k)
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

static int join_path_local(char *dst, size_t dst_sz, const char *dir, const char *name)
{
  int n = snprintf(dst, dst_sz, "%s/%s", dir, name);
  return (n < 0 || (size_t)n >= dst_sz) ? -1 : 0;
}

void report_write_run_status(const char *run_dir, const char *status, const char *stage, const char *message, const RunMessages *msgs)
{
  char p[PATH_MAX];
  if (join_path_local(p, sizeof(p), run_dir, "run_status.json") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;
  fprintf(f, "{\n  \"status\": \"%s\",\n  \"stage\": \"%s\",\n  \"message\": \"%s\",\n  \"warnings\": %d,\n  \"errors\": %d,\n  \"warning_messages\": [",
          status ? status : "unknown",
          stage ? stage : "unknown",
          message ? message : "",
          msgs ? msgs->warning_count : 0,
          msgs ? msgs->error_count : 0);
  if (msgs)
  {
    for (int i = 0; i < msgs->warning_count; ++i)
      fprintf(f, "%s\"%s\"", (i ? "," : ""), msgs->warnings[i]);
  }
  fprintf(f, "],\n  \"error_messages\": [");
  if (msgs)
  {
    for (int i = 0; i < msgs->error_count; ++i)
      fprintf(f, "%s\"%s\"", (i ? "," : ""), msgs->errors[i]);
  }
  fprintf(f, "]\n}\n");
  fclose(f);

  char tp[PATH_MAX];
  if (join_path_local(tp, sizeof(tp), run_dir, "run_timeline.ndjson") == 0)
  {
    FILE *tf = fopen(tp, "a");
    if (tf)
    {
      time_t ts = time(NULL);
      fprintf(tf, "{\"ts\":%lld,\"status\":\"%s\",\"stage\":\"%s\",\"message\":\"%s\"}\n",
              (long long)ts, status ? status : "unknown", stage ? stage : "unknown", message ? message : "");
      fclose(tf);
    }
  }
}

void report_write_scenario_json(const char *run_dir, const Scenario *sc)
{
  char p[PATH_MAX];
  if (join_path_local(p, sizeof(p), run_dir, "scenario.json") != 0)
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
            kind_name_local(st->kind),
            st->duration_sec,
            st->threads,
            st->arg ? st->arg : "",
            (i + 1 < sc->step_count) ? "," : "");
  }

  fprintf(f, "  ]\n}\n");
  fclose(f);
}

void report_write_system_info(const char *run_dir)
{
  char p[PATH_MAX];
  if (join_path_local(p, sizeof(p), run_dir, "system_info.json") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;

  char host[256] = "unknown";
#ifdef _WIN32
  DWORD host_sz = (DWORD)(sizeof(host) - 1);
  if (!GetComputerNameA(host, &host_sz))
    snprintf(host, sizeof(host), "unknown");
#else
  gethostname(host, sizeof(host) - 1);
#endif

  char kernel[512] = "unknown";
  char cpu_model[256] = "unknown";
  char mem_total_kb[64] = "unknown";
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

  FILE *cf = fopen("/proc/cpuinfo", "r");
  if (cf)
  {
    char line[512];
    while (fgets(line, sizeof(line), cf))
    {
      if (strncmp(line, "model name", 10) == 0 || strncmp(line, "Hardware", 8) == 0)
      {
        char *p = strchr(line, ':');
        if (p)
        {
          p++;
          while (*p == ' ' || *p == '\t')
            p++;
          snprintf(cpu_model, sizeof(cpu_model), "%s", p);
          size_t len = strlen(cpu_model);
          while (len > 0 && (cpu_model[len - 1] == '\n' || cpu_model[len - 1] == '\r'))
            cpu_model[--len] = '\0';
          break;
        }
      }
    }
    fclose(cf);
  }

  FILE *mf = fopen("/proc/meminfo", "r");
  if (mf)
  {
    char line[256];
    while (fgets(line, sizeof(line), mf))
    {
      if (strncmp(line, "MemTotal:", 9) == 0)
      {
        char *p = line + 9;
        while (*p == ' ' || *p == '\t')
          p++;
        size_t cp = strcspn(p, "\r\n");
        if (cp >= sizeof(mem_total_kb))
          cp = sizeof(mem_total_kb) - 1;
        memcpy(mem_total_kb, p, cp);
        mem_total_kb[cp] = '\0';
        size_t len = strlen(mem_total_kb);
        while (len > 0 && (mem_total_kb[len - 1] == '\n' || mem_total_kb[len - 1] == '\r'))
          mem_total_kb[--len] = '\0';
        break;
      }
    }
    fclose(mf);
  }

  fprintf(f, "{\n");
  fprintf(f, "  \"hostname\": \"%s\",\n", host);
  fprintf(f, "  \"kernel\": \"%s\",\n", kernel);
  fprintf(f, "  \"cpu_model\": \"%s\",\n", cpu_model);
  fprintf(f, "  \"mem_total\": \"%s\"\n", mem_total_kb);
  fprintf(f, "}\n");
  fclose(f);
}

void report_write_metrics_json(const char *run_dir, const Scenario *sc, const Collector *c, const StepResult *res, int nres)
{
  char p[PATH_MAX];
  if (join_path_local(p, sizeof(p), run_dir, "metrics.json") != 0)
    return;
  FILE *f = fopen(p, "w");
  if (!f)
    return;

  SeriesLocal freq, temp;
  s_init(&freq, c->nrows);
  s_init(&temp, c->nrows);

  for (size_t i = 0; i < c->nrows; ++i)
  {
    if (c->rows[i].cpu_freq_mhz > 0)
      s_push(&freq, c->rows[i].cpu_freq_mhz);
    if (c->rows[i].temp_c > 0)
      s_push(&temp, c->rows[i].temp_c);
  }

  int throttle_hint = 0;
  if (freq.n > 2 && temp.n > 2)
    throttle_hint = detect_freq_drop_with_temp_rise(freq.v, temp.v, freq.n < temp.n ? freq.n : temp.n, 10.0, 5.0);

  double cpu_avg = -1.0, nn_avg = -1.0, mem_copy_avg = -1.0, storage_avg = -1.0, ping_p95_avg = -1.0, jitter_p99_avg = -1.0;
  int c_cpu = 0, c_nn = 0, c_mem = 0, c_st = 0, c_net = 0, c_jit = 0;
  double cpu_windows[MAX_STEPS];
  size_t cpu_windows_n = 0;
  double storage_p99_vals[MAX_STEPS];
  size_t storage_p99_n = 0;
  double network_avg_vals[MAX_STEPS];
  size_t network_avg_n = 0;
  double jitter_p99_vals[MAX_STEPS];
  size_t jitter_p99_n = 0;
  uint64_t jitter_over500_sum = 0, jitter_over1000_sum = 0;
  double network_min_vals[MAX_STEPS], network_max_vals[MAX_STEPS], network_loss_vals[MAX_STEPS];
  size_t network_min_n = 0, network_max_n = 0, network_loss_n = 0;
  uint64_t network_errors_sum = 0;
  double storage_avg_lat_vals[MAX_STEPS], storage_p95_lat_vals[MAX_STEPS], storage_p999_vals[MAX_STEPS], storage_max_vals[MAX_STEPS];
  size_t storage_avg_lat_n = 0, storage_p95_lat_n = 0, storage_p999_n = 0, storage_max_n = 0;
  uint64_t storage_outliers_sum = 0;
  double jitter_avg_vals[MAX_STEPS], jitter_p95_vals[MAX_STEPS], jitter_max_vals[MAX_STEPS];
  size_t jitter_avg_n = 0, jitter_p95_n = 0, jitter_max_n = 0;

  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind == WK_CPU_BURN && res[i].ops_per_sec > 0)
    {
      cpu_avg = (cpu_avg < 0 ? 0 : cpu_avg) + res[i].ops_per_sec;
      c_cpu++;
      if (cpu_windows_n < MAX_STEPS)
        cpu_windows[cpu_windows_n++] = res[i].ops_window_end;
    }
    if (res[i].step->kind == WK_NN && res[i].nn_inf_per_sec > 0)
    {
      nn_avg = (nn_avg < 0 ? 0 : nn_avg) + res[i].nn_inf_per_sec;
      c_nn++;
    }
    if (res[i].step->kind == WK_MEMORY && res[i].mem_copy_mb_s > 0)
    {
      mem_copy_avg = (mem_copy_avg < 0 ? 0 : mem_copy_avg) + res[i].mem_copy_mb_s;
      c_mem++;
    }
    if (res[i].step->kind == WK_STORAGE && res[i].throughput_mb_s > 0)
    {
      storage_avg = (storage_avg < 0 ? 0 : storage_avg) + res[i].throughput_mb_s;
      c_st++;
      if (storage_p99_n < MAX_STEPS && res[i].storage_lat_p99_us > 0)
        storage_p99_vals[storage_p99_n++] = res[i].storage_lat_p99_us;
      if (storage_avg_lat_n < MAX_STEPS && res[i].storage_lat_avg_us > 0)
        storage_avg_lat_vals[storage_avg_lat_n++] = res[i].storage_lat_avg_us;
      if (storage_p95_lat_n < MAX_STEPS && res[i].storage_lat_p95_us > 0)
        storage_p95_lat_vals[storage_p95_lat_n++] = res[i].storage_lat_p95_us;
      if (storage_p999_n < MAX_STEPS && res[i].storage_lat_p999_us > 0)
        storage_p999_vals[storage_p999_n++] = res[i].storage_lat_p999_us;
      if (storage_max_n < MAX_STEPS && res[i].storage_lat_max_us > 0)
        storage_max_vals[storage_max_n++] = res[i].storage_lat_max_us;
      storage_outliers_sum += res[i].storage_outliers;
    }
    if (res[i].step->kind == WK_PING && res[i].ping_p95_ms > 0)
    {
      ping_p95_avg = (ping_p95_avg < 0 ? 0 : ping_p95_avg) + res[i].ping_p95_ms;
      c_net++;
      if (network_avg_n < MAX_STEPS && res[i].ping_avg_ms > 0)
        network_avg_vals[network_avg_n++] = res[i].ping_avg_ms;
      if (network_min_n < MAX_STEPS && res[i].ping_min_ms >= 0)
        network_min_vals[network_min_n++] = res[i].ping_min_ms;
      if (network_max_n < MAX_STEPS && res[i].ping_max_ms >= 0)
        network_max_vals[network_max_n++] = res[i].ping_max_ms;
      if (network_loss_n < MAX_STEPS && res[i].packet_loss_pct >= 0)
        network_loss_vals[network_loss_n++] = res[i].packet_loss_pct;
      network_errors_sum += res[i].ping_errors;
    }
    if (res[i].step->kind == WK_JITTER && res[i].jitter_p99_us > 0)
    {
      jitter_p99_avg = (jitter_p99_avg < 0 ? 0 : jitter_p99_avg) + res[i].jitter_p99_us;
      c_jit++;
      if (jitter_p99_n < MAX_STEPS)
        jitter_p99_vals[jitter_p99_n++] = res[i].jitter_p99_us;
      if (jitter_avg_n < MAX_STEPS && res[i].jitter_avg_us > 0)
        jitter_avg_vals[jitter_avg_n++] = res[i].jitter_avg_us;
      if (jitter_p95_n < MAX_STEPS && res[i].jitter_p95_us > 0)
        jitter_p95_vals[jitter_p95_n++] = res[i].jitter_p95_us;
      if (jitter_max_n < MAX_STEPS && res[i].jitter_max_us > 0)
        jitter_max_vals[jitter_max_n++] = res[i].jitter_max_us;
      jitter_over500_sum += res[i].jitter_over_500us;
      jitter_over1000_sum += res[i].jitter_over_1000us;
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

  StatsSummary storage_p99_stats = stats_from_array(storage_p99_vals, storage_p99_n);
  StatsSummary storage_avg_lat_stats = stats_from_array(storage_avg_lat_vals, storage_avg_lat_n);
  StatsSummary storage_p95_lat_stats = stats_from_array(storage_p95_lat_vals, storage_p95_lat_n);
  StatsSummary storage_p999_stats = stats_from_array(storage_p999_vals, storage_p999_n);
  StatsSummary storage_max_stats = stats_from_array(storage_max_vals, storage_max_n);
  StatsSummary network_avg_stats = stats_from_array(network_avg_vals, network_avg_n);
  StatsSummary network_min_stats = stats_from_array(network_min_vals, network_min_n);
  StatsSummary network_max_stats = stats_from_array(network_max_vals, network_max_n);
  StatsSummary network_loss_stats = stats_from_array(network_loss_vals, network_loss_n);
  StatsSummary jitter_p99_stats = stats_from_array(jitter_p99_vals, jitter_p99_n);
  StatsSummary jitter_avg_stats = stats_from_array(jitter_avg_vals, jitter_avg_n);
  StatsSummary jitter_p95_stats = stats_from_array(jitter_p95_vals, jitter_p95_n);
  StatsSummary jitter_max_stats = stats_from_array(jitter_max_vals, jitter_max_n);
  double cpu_stability = calc_stability_coeff(cpu_windows, cpu_windows_n);

  fprintf(f, "{\n");
  fprintf(f, "  \"scenario\": \"%s\",\n", sc->name ? sc->name : "unknown");
  fprintf(f, "  \"cpu_ops_avg\": %.3f,\n", cpu_avg);
  fprintf(f, "  \"nn_inf_per_sec_avg\": %.3f,\n", nn_avg);
  fprintf(f, "  \"mem_copy_mb_s_avg\": %.3f,\n", mem_copy_avg);
  fprintf(f, "  \"storage_mb_s_avg\": %.3f,\n", storage_avg);
  fprintf(f, "  \"ping_p95_ms_avg\": %.3f,\n", ping_p95_avg);
  fprintf(f, "  \"jitter_p99_us_avg\": %.3f,\n", jitter_p99_avg);
  fprintf(f, "  \"storage_p99_us_median\": %.3f,\n", storage_p99_stats.median);
  fprintf(f, "  \"storage_p99_us_p95\": %.3f,\n", storage_p99_stats.p95);
  fprintf(f, "  \"storage_p99_us_max\": %.3f,\n", storage_p99_stats.max);
  fprintf(f, "  \"storage_lat_avg_us\": %.3f,\n", storage_avg_lat_stats.avg);
  fprintf(f, "  \"storage_lat_p95_us\": %.3f,\n", storage_p95_lat_stats.avg);
  fprintf(f, "  \"storage_lat_p999_us\": %.3f,\n", storage_p999_stats.avg);
  fprintf(f, "  \"storage_lat_max_us\": %.3f,\n", storage_max_stats.max);
  fprintf(f, "  \"storage_outliers_total\": %" PRIu64 ",\n", storage_outliers_sum);
  fprintf(f, "  \"network_rtt_min_ms\": %.3f,\n", network_min_stats.avg);
  fprintf(f, "  \"network_rtt_avg_ms_median\": %.3f,\n", network_avg_stats.median);
  fprintf(f, "  \"network_rtt_avg_ms_p95\": %.3f,\n", network_avg_stats.p95);
  fprintf(f, "  \"network_rtt_max_ms\": %.3f,\n", network_max_stats.avg);
  fprintf(f, "  \"network_loss_pct\": %.3f,\n", network_loss_stats.avg);
  fprintf(f, "  \"network_errors_total\": %" PRIu64 ",\n", network_errors_sum);
  fprintf(f, "  \"jitter_avg_us\": %.3f,\n", jitter_avg_stats.avg);
  fprintf(f, "  \"jitter_p95_us\": %.3f,\n", jitter_p95_stats.avg);
  fprintf(f, "  \"jitter_p99_us_median\": %.3f,\n", jitter_p99_stats.median);
  fprintf(f, "  \"jitter_max_us\": %.3f,\n", jitter_max_stats.max);
  fprintf(f, "  \"jitter_over_500us_total\": %" PRIu64 ",\n", jitter_over500_sum);
  fprintf(f, "  \"jitter_over_1000us_total\": %" PRIu64 ",\n", jitter_over1000_sum);
  fprintf(f, "  \"cpu_stability_score\": %.3f,\n", cpu_stability);
  fprintf(f, "  \"throttle_hint\": %d\n", throttle_hint);
  fprintf(f, "}\n");
  fclose(f);

  s_free(&freq);
  s_free(&temp);
}

void report_write_report_md(const char *run_dir, const Scenario *sc, const Collector *c, const StepResult *res, int nres,
                            const RunMessages *msgs, const RunContext *run_ctx, double run_sec)
{
  char p[PATH_MAX];
  if (join_path_local(p, sizeof(p), run_dir, "report.md") != 0)
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

  int warn_thermal = 0, warn_freq = 0, warn_psi = 0;
  if (msgs)
  {
    for (int i = 0; i < msgs->warning_count; ++i)
    {
      if (strstr(msgs->warnings[i], "thermal"))
        warn_thermal = 1;
      if (strstr(msgs->warnings[i], "cpu frequency"))
        warn_freq = 1;
      if (strstr(msgs->warnings[i], "PSI"))
        warn_psi = 1;
    }
  }
  fprintf(f, "## Telemetry availability\n\n");
  fprintf(f, "- thermal: %s\n", warn_thermal ? "unavailable or limited" : "ok");
  fprintf(f, "- cpu frequency: %s\n", warn_freq ? "unavailable or limited" : "ok");
  fprintf(f, "- PSI: %s\n\n", warn_psi ? "unavailable or limited" : "ok");

  fprintf(f, "## Steps\n\n");
  fprintf(f, "| Step | Kind | CPU ops/s | NN inf/s | Mem copy MB/s | Storage MB/s | Net p95 ms | Jitter p99 us |\n");
  fprintf(f, "|---|---|---:|---:|---:|---:|---:|---:|\n");
  for (int i = 0; i < nres; ++i)
  {
    fprintf(f, "| %s | %s | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f |\n",
            res[i].step->name,
            kind_name_local(res[i].step->kind),
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

  const char *verdict = "OK";
  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind == WK_JITTER && res[i].jitter_p99_us > 1000.0)
      verdict = "WARN";
    if (res[i].step->kind == WK_PING && res[i].packet_loss_pct > 3.0)
      verdict = "WARN";
    if (res[i].step->kind == WK_STORAGE && res[i].storage_lat_p99_us > 50000.0)
      verdict = "WARN";
  }
  fprintf(f, "\n## Verdict\n\n");
  fprintf(f, "- Result: **%s**\n", verdict);
  if (strcmp(verdict, "WARN") == 0)
    fprintf(f, "- Причина: одна или несколько tail-метрик вышли за эвристический порог.\n");
  else
    fprintf(f, "- Основные хвостовые метрики в допустимом эвристическом диапазоне.\n");

  fclose(f);
}

void report_write_step_csvs(const char *run_dir, const StepResult *res, int nres)
{
  char p[PATH_MAX];
  FILE *f = NULL;

  if (join_path_local(p, sizeof(p), run_dir, "cpu.csv") == 0 && (f = fopen(p, "w")))
  {
    fprintf(f, "step,ops_per_sec,window_start_ops,window_end_ops,degradation_pct\n");
    for (int i = 0; i < nres; ++i)
      if (res[i].step->kind == WK_CPU_BURN)
        fprintf(f, "%s,%.3f,%.3f,%.3f,%.3f\n", res[i].step->name, res[i].ops_per_sec, res[i].ops_window_start, res[i].ops_window_end, res[i].cpu_degradation_pct);
    fclose(f);
  }

  if (join_path_local(p, sizeof(p), run_dir, "memory.csv") == 0 && (f = fopen(p, "w")))
  {
    fprintf(f, "step,read_mb_s,write_mb_s,copy_mb_s\n");
    for (int i = 0; i < nres; ++i)
      if (res[i].step->kind == WK_MEMORY)
        fprintf(f, "%s,%.3f,%.3f,%.3f\n", res[i].step->name, res[i].mem_read_mb_s, res[i].mem_write_mb_s, res[i].mem_copy_mb_s);
    fclose(f);
  }

  if (join_path_local(p, sizeof(p), run_dir, "storage.csv") == 0 && (f = fopen(p, "w")))
  {
    fprintf(f, "step,throughput_mb_s,iops,lat_avg_us,lat_p50_us,lat_p95_us,lat_p99_us,lat_p999_us,lat_max_us,outliers\n");
    for (int i = 0; i < nres; ++i)
      if (res[i].step->kind == WK_STORAGE)
        fprintf(f, "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu64 "\n", res[i].step->name, res[i].throughput_mb_s, res[i].storage_iops, res[i].storage_lat_avg_us, res[i].storage_lat_p50_us, res[i].storage_lat_p95_us, res[i].storage_lat_p99_us, res[i].storage_lat_p999_us, res[i].storage_lat_max_us, res[i].storage_outliers);
    fclose(f);
  }

  if (join_path_local(p, sizeof(p), run_dir, "network.csv") == 0 && (f = fopen(p, "w")))
  {
    fprintf(f, "step,loss_pct,p95_ms,p99_ms,min_ms,avg_ms,max_ms,errors\n");
    for (int i = 0; i < nres; ++i)
      if (res[i].step->kind == WK_PING)
        fprintf(f, "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu64 "\n", res[i].step->name, res[i].packet_loss_pct, res[i].ping_p95_ms, res[i].ping_p99_ms, res[i].ping_min_ms, res[i].ping_avg_ms, res[i].ping_max_ms, res[i].ping_errors);
    fclose(f);
  }

  if (join_path_local(p, sizeof(p), run_dir, "jitter.csv") == 0 && (f = fopen(p, "w")))
  {
    fprintf(f, "step,avg_us,p50_us,p95_us,p99_us,max_us,over_500us,over_1000us\n");
    for (int i = 0; i < nres; ++i)
      if (res[i].step->kind == WK_JITTER)
        fprintf(f, "%s,%.3f,%.3f,%.3f,%.3f,%.3f,%" PRIu64 ",%" PRIu64 "\n", res[i].step->name, res[i].jitter_avg_us, res[i].jitter_p50_us, res[i].jitter_p95_us, res[i].jitter_p99_us, res[i].jitter_max_us, res[i].jitter_over_500us, res[i].jitter_over_1000us);
    fclose(f);
  }
}