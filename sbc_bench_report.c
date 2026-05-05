#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "sbc_bench_report.h"
#include "sbc_bench_metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  fprintf(f, "{\n  \"status\": \"%s\",\n  \"stage\": \"%s\",\n  \"message\": \"%s\",\n  \"warnings\": %d,\n  \"errors\": %d\n}\n",
          status ? status : "unknown",
          stage ? stage : "unknown",
          message ? message : "",
          msgs ? msgs->warning_count : 0,
          msgs ? msgs->error_count : 0);
  fclose(f);
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

  for (int i = 0; i < nres; ++i)
  {
    if (res[i].step->kind == WK_CPU_BURN && res[i].ops_per_sec > 0)
    {
      cpu_avg = (cpu_avg < 0 ? 0 : cpu_avg) + res[i].ops_per_sec;
      c_cpu++;
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
    }
    if (res[i].step->kind == WK_PING && res[i].ping_p95_ms > 0)
    {
      ping_p95_avg = (ping_p95_avg < 0 ? 0 : ping_p95_avg) + res[i].ping_p95_ms;
      c_net++;
    }
    if (res[i].step->kind == WK_JITTER && res[i].jitter_p99_us > 0)
    {
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

  fclose(f);
}