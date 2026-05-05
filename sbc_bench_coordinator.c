#include "sbc_bench_coordinator.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int coordinator_prepare_run(const Scenario *sc, int replace_latest, RunContext *ctx)
{
  if (!sc || !sc->name || !ctx)
    return -1;

  memset(ctx, 0, sizeof(*ctx));
  ctx->created_at = time(NULL);

  struct tm tmv;
#ifdef _WIN32
  localtime_s(&tmv, &ctx->created_at);
#else
  localtime_r(&ctx->created_at, &tmv);
#endif

  snprintf(ctx->run_id, sizeof(ctx->run_id), "%s_%04d%02d%02d_%02d%02d%02d",
           sc->name, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

  if (replace_latest)
    snprintf(ctx->run_dir, sizeof(ctx->run_dir), "runs_v4_c/%s_latest", sc->name);
  else
    snprintf(ctx->run_dir, sizeof(ctx->run_dir), "runs_v4_c/%s", ctx->run_id);
  return 0;
}

int coordinator_write_run_id(const RunContext *ctx)
{
  if (!ctx || !ctx->run_dir[0] || !ctx->run_id[0])
    return -1;

  char p[PATH_MAX];
  int n = snprintf(p, sizeof(p), "%s/run_id.txt", ctx->run_dir);
  if (n < 0 || (size_t)n >= sizeof(p))
    return -1;
  FILE *f = fopen(p, "w");
  if (!f)
    return -1;
  fprintf(f, "%s\n", ctx->run_id);
  fclose(f);
  return 0;
}