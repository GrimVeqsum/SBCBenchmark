#ifndef SBC_BENCH_NOISE_H
#define SBC_BENCH_NOISE_H

#include "sbc_bench_types.h"

typedef struct NoiseContext NoiseContext;

/* Запуск фоновой помехи в соответствии с режимом */
NoiseContext *noise_start(NoiseMode mode, int cpu_threads, const char *io_dir);

/* Остановка и освобождение */
void noise_stop(NoiseContext *ctx);

#endif