#ifndef SBC_BENCH_COORDINATOR_H
#define SBC_BENCH_COORDINATOR_H

#include "sbc_bench_types.h"

typedef struct
{
  char run_id[64];
  char run_dir[PATH_MAX];
  time_t created_at;
  double duration_scale;
} RunContext;

int coordinator_prepare_run(const Scenario *sc, int replace_latest, RunContext *ctx);
int coordinator_write_run_id(const RunContext *ctx);

#endif