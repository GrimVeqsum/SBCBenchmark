#include "sbc_bench_v4.h"

static uint64_t now_ns_local(void) {
  struct timespec t;
#ifdef CLOCK_MONOTONIC_RAW
  clock_gettime(CLOCK_MONOTONIC_RAW, &t);
#else
  clock_gettime(CLOCK_MONOTONIC, &t);
#endif
  return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static double sec_from_ns(uint64_t ns) { return (double)ns / 1e9; }

static uint64_t xorshift64(uint64_t *s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

typedef struct {
  volatile int stop;
  uint64_t ops;
} CpuWorkerCtx;

static void* cpu_worker(void *arg) {
  CpuWorkerCtx *ctx = (CpuWorkerCtx*)arg;
  uint64_t s = 0x123456789abcdefULL;
  while (!ctx->stop) {
    for (int i=0; i<1024; ++i) {
      s ^= s << 1;
      s += 0x9e3779b97f4a7c15ULL;
      s ^= s >> 3;
      s *= 0xbf58476d1ce4e5b9ULL;
    }
    ctx->ops += 1024;
  }
  return NULL;
}

StepResult workload_run_cpu_burn(int duration_s, int threads) {
  if (threads < 1) threads = 1;
  StepResult r = {0};
  r.duration_s = (double)duration_s;

  CpuWorkerCtx *ctx = (CpuWorkerCtx*)calloc((size_t)threads, sizeof(*ctx));
  pthread_t *th = (pthread_t*)calloc((size_t)threads, sizeof(*th));
  if (!ctx || !th) {
    free(ctx); free(th);
    r.status = -1;
    return r;
  }

  uint64_t t0 = now_ns_local();
  for (int i=0; i<threads; ++i) pthread_create(&th[i], NULL, cpu_worker, &ctx[i]);

  sleep((unsigned)duration_s);

  for (int i=0; i<threads; ++i) ctx[i].stop = 1;
  for (int i=0; i<threads; ++i) pthread_join(th[i], NULL);
  uint64_t t1 = now_ns_local();

  uint64_t total_ops = 0;
  for (int i=0; i<threads; ++i) total_ops += ctx[i].ops;

  double elapsed = sec_from_ns(t1 - t0);
  if (elapsed <= 0.0) elapsed = (double)duration_s;
  r.ops_per_sec = (double)total_ops / elapsed;
  r.mb_per_sec = -1.0;
  r.lat_p95_ms = -1.0;
  r.lat_p99_ms = -1.0;
  r.jitter_p99_us = -1.0;
  r.status = 0;

  free(ctx); free(th);
  return r;
}

static size_t clamp_size(size_t v, size_t lo, size_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

StepResult workload_run_memory_test(int duration_s, size_t bytes, int *clamped_out) {
  StepResult r = {0};
  r.duration_s = (double)duration_s;
  if (clamped_out) *clamped_out = 0;

  size_t req = bytes;
  size_t minb = 1u << 20;    /* 1 MiB */
  size_t maxb = 1u << 30;    /* 1 GiB */
  size_t n = clamp_size(req ? req : (64u<<20), minb, maxb);
  if (req != n && clamped_out) *clamped_out = 1;

  uint8_t *a = (uint8_t*)malloc(n);
  uint8_t *b = (uint8_t*)malloc(n);
  uint8_t *c = (uint8_t*)malloc(n);
  if (!a || !b || !c) {
    free(a); free(b); free(c);
    r.status = -1;
    return r;
  }

  memset(a, 0xA5, n);
  memset(b, 0x5A, n);

  uint64_t t0 = now_ns_local();
  uint64_t end = t0 + (uint64_t)duration_s * 1000000000ULL;
  uint64_t bytes_done = 0;
  while (now_ns_local() < end) {
    memcpy(c, a, n);
    memcpy(a, b, n);
    memcpy(b, c, n);
    bytes_done += (uint64_t)n * 3ULL;
  }
  uint64_t t1 = now_ns_local();

  double elapsed = sec_from_ns(t1 - t0);
  if (elapsed <= 0.0) elapsed = (double)duration_s;
  r.mb_per_sec = (double)bytes_done / elapsed / (1024.0*1024.0);
  r.ops_per_sec = -1.0;
  r.lat_p95_ms = -1.0;
  r.lat_p99_ms = -1.0;
  r.jitter_p99_us = -1.0;
  r.status = 0;

  free(a); free(b); free(c);
  return r;
}

StepResult workload_run_jitter_test(int duration_s, int interval_ms) {
  StepResult r = {0};
  r.duration_s = (double)duration_s;
  if (interval_ms < 1) interval_ms = 1;

  int n = duration_s * 1000 / interval_ms;
  if (n < 10) n = 10;
  double *jit_us = (double*)malloc((size_t)n * sizeof(double));
  if (!jit_us) { r.status = -1; return r; }

  struct timespec req;
  req.tv_sec = interval_ms / 1000;
  req.tv_nsec = (long)(interval_ms % 1000) * 1000000L;

  uint64_t t_prev = now_ns_local();
  for (int i=0; i<n; ++i) {
    nanosleep(&req, NULL);
    uint64_t t_now = now_ns_local();
    double dt_ms = (double)(t_now - t_prev) / 1e6;
    double j_us = (dt_ms - (double)interval_ms) * 1000.0;
    if (j_us < 0) j_us = -j_us;
    jit_us[i] = j_us;
    t_prev = t_now;
  }

  Stats st = stats_from_array(jit_us, (size_t)n);
  r.jitter_p99_us = st.p99;
  r.ops_per_sec = -1.0;
  r.mb_per_sec = -1.0;
  r.lat_p95_ms = -1.0;
  r.lat_p99_ms = -1.0;
  r.status = 0;

  free(jit_us);
  return r;
}

/* Lightweight NN-like matrix multiply, returns inferences per second (higher is better). */
double workload_run_nn_inference(int duration_s) {
  int n = 128;
  const int min_n = 16;
  const double safe_frac = 0.6; /* only use part of currently available memory */

  FILE *mf = fopen("/proc/meminfo", "r");
  if (mf) {
    long long mem_avail_kb = -1;
    char key[64], unit[32];
    long long val;
    while (fscanf(mf, "%63s %lld %31s", key, &val, unit) == 3) {
      if (strcmp(key, "MemAvailable:") == 0) {
        mem_avail_kb = val;
        break;
      }
    }
    fclose(mf);
    if (mem_avail_kb > 0) {
      double avail_bytes = (double)mem_avail_kb * 1024.0 * safe_frac;
      double bytes_per_n = 3.0 * sizeof(float); /* A,B,C => 3*n*n floats */
      int fit_n = (int)floor(sqrt(avail_bytes / bytes_per_n));
      if (fit_n < n) n = fit_n;
      if (n < min_n) return -2.0; /* signal "skipped due to low memory" */
    }
  }

  size_t sz = (size_t)n * (size_t)n;
  float *A = (float*)malloc(sz * sizeof(float));
  float *B = (float*)malloc(sz * sizeof(float));
  float *C = (float*)malloc(sz * sizeof(float));
  if (!A || !B || !C) { free(A); free(B); free(C); return -1.0; }

  for (size_t i=0; i<sz; ++i) {
    A[i] = (float)((i % 13) * 0.1f);
    B[i] = (float)((i % 7) * 0.2f);
    C[i] = 0.0f;
  }

  uint64_t t0 = now_ns_local();
  uint64_t end = t0 + (uint64_t)duration_s * 1000000000ULL;
  uint64_t inf = 0;

  while (now_ns_local() < end) {
    for (int i=0; i<n; ++i) {
      for (int j=0; j<n; ++j) {
        float s = 0.0f;
        for (int k=0; k<n; ++k) s += A[(size_t)i*n + k] * B[(size_t)k*n + j];
        C[(size_t)i*n + j] = s;
      }
    }
    inf++;
  }

  uint64_t t1 = now_ns_local();
  double elapsed = sec_from_ns(t1 - t0);
  free(A); free(B); free(C);
  if (elapsed <= 0.0) return -1.0;
  return (double)inf / elapsed;
}