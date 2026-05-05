#ifndef SBC_BENCH_REPORT_H
#define SBC_BENCH_REPORT_H

#include "sbc_bench_types.h"
#include "sbc_bench_coordinator.h"

void report_write_run_status(const char *run_dir, const char *status, const char *stage, const char *message, const RunMessages *msgs);
void report_write_scenario_json(const char *run_dir, const Scenario *sc);
void report_write_system_info(const char *run_dir);
void report_write_metrics_json(const char *run_dir, const Scenario *sc, const Collector *c, const StepResult *res, int nres);
void report_write_report_md(const char *run_dir, const Scenario *sc, const Collector *c, const StepResult *res, int nres,
                            const RunMessages *msgs, const RunContext *run_ctx, double run_sec);

#endif