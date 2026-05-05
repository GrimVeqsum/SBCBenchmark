#include "sbc_bench_network.h"
#include "sbc_bench_metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int join_path_local(char *dst, size_t dst_sz, const char *dir, const char *name)
{
  int n = snprintf(dst, dst_sz, "%s/%s", dir, name);
  return (n < 0 || (size_t)n >= dst_sz) ? -1 : 0;
}

void network_run_ping(const Step *s,
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

  SeriesLocal rtts;
  s_init(&rtts, MAX_STAT_POINTS);

  char detail_csv[PATH_MAX];
  FILE *df = NULL;
  if (out_dir && join_path_local(detail_csv, sizeof(detail_csv), out_dir, "network_detail.csv") == 0)
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
          s_push(&rtts, v);
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

  s_free(&rtts);
}