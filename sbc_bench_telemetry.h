#ifndef SBC_BENCH_TELEMETRY_H
#define SBC_BENCH_TELEMETRY_H

#include "sbc_bench_types.h"

void telemetry_detect_channel_warnings(RunMessages *msgs);

void telemetry_collector_init(Collector *c, Row *rows, size_t cap, int sample_sec, const char *out_dir);
void *telemetry_collector_thread(void *arg);

void telemetry_write_csv(const Collector *c);

#endif