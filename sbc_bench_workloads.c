#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "sbc_bench_workloads.h"
#include "sbc_bench_metrics.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <malloc.h>
#endif

typedef struct
{
  volatile sig_atomic_t *stop;
  volatile sig_atomic_t *global_stop;
  uint64_t *counter;
} BurnArgs;

typedef struct
{
  double *v;
  size_t n;
  size_t cap;
} Series;

static void series_init(Series *s, size_t cap)
{
  s->n = 0;
  s->cap = cap;
  s->v = (double *)calloc(cap ? cap : 1, sizeof(double));
}
static void series_push(Series *s, double x)
{
  if (!s || !s->v || s->n >= s->cap)
    return;
  s->v[s->n++] = x;
}
static void series_free(Series *s)
{
  if (!s)
    return;
  free(s->v);
  s->v = NULL;
  s->n = s->cap = 0;
}

static double now_sec_local(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *burn_worker(void *arg)
{
  BurnArgs *a = (BurnArgs *)arg;
  uint64_t c = 0;
  uint32_t x = 1u;
  while (!*(a->stop) && !*(a->global_stop))
  {
    for (int i = 0; i < 20000; ++i)
    {
      x = x * 1664525u + 1013904223u;
      c++;
    }
    *(a->counter) = c;
  }
  (void)x;
  return NULL;
}

void workload_run_cpu_burn(const Step *s, volatile sig_atomic_t *g_stop,
                           double *ops_per_sec, double *window_start_ops, double *window_end_ops, double *degradation_pct)
{
  int n = s->threads > 0 ? s->threads : 1;
  if (n > MAX_CPUS)
    n = MAX_CPUS;

  pthread_t th[MAX_CPUS];
  BurnArgs args[MAX_CPUS];
  uint64_t counters[MAX_CPUS];
  memset(counters, 0, sizeof(counters));

  volatile sig_atomic_t stop = 0;
  for (int i = 0; i < n; ++i)
  {
    args[i].stop = &stop;
    args[i].global_stop = g_stop;
    args[i].counter = &counters[i];
    pthread_create(&th[i], NULL, burn_worker, &args[i]);
  }

  double t0 = now_sec_local();
  double mid = t0 + s->duration_sec * 0.5;
  uint64_t mid_ops = 0, end_ops = 0;

  while (!(*g_stop) && now_sec_local() - t0 < s->duration_sec)
  {
    uint64_t sum = 0;
    for (int i = 0; i < n; ++i)
      sum += counters[i];

    if (now_sec_local() < mid)
      mid_ops = sum;
    end_ops = sum;
    usleep(100000);
  }

  stop = 1;
  for (int i = 0; i < n; ++i)
    pthread_join(th[i], NULL);

  double elapsed = now_sec_local() - t0;
  if (elapsed <= 0)
    elapsed = 1e-6;

  const double total_ops = (double)end_ops;
  *ops_per_sec = total_ops / elapsed;
  const double first_window = (double)mid_ops / (elapsed * 0.5 > 0 ? elapsed * 0.5 : 1e-6);
  const double second_window = (double)(end_ops - mid_ops) / (elapsed * 0.5 > 0 ? elapsed * 0.5 : 1e-6);
  *window_start_ops = first_window;
  *window_end_ops = second_window;
  *degradation_pct = calc_degradation_percent(first_window, second_window);
}

double workload_run_nn_inference(const Step *s, volatile sig_atomic_t *g_stop)
{
  int n = s->threads > 0 ? s->threads : 1;
  int dim = 32;
  if (s->arg)
  {
    int v = atoi(s->arg);
    if (v >= 8 && v <= 128)
      dim = v;
  }

  size_t sz = (size_t)dim * (size_t)dim;
  FILE *mf = fopen("/proc/meminfo", "r");
  if (mf)
  {
    char line[256];
    long long mem_avail_kb = -1;
    while (fgets(line, sizeof(line), mf))
    {
      if (sscanf(line, "MemAvailable: %lld kB", &mem_avail_kb) == 1)
        break;
    }
    fclose(mf);
    if (mem_avail_kb > 0)
    {
      size_t safe_bytes = (size_t)((mem_avail_kb * 1024LL) / 10LL);
      while ((3ULL * sz * sizeof(float)) > safe_bytes && dim > 8)
      {
        dim -= 4;
        sz = (size_t)dim * (size_t)dim;
      }
      if ((3ULL * sz * sizeof(float)) > safe_bytes)
        return -2.0;
    }
  }

  float *A = (float *)malloc(sz * sizeof(float));
  float *B = (float *)malloc(sz * sizeof(float));
  float *C = (float *)malloc(sz * sizeof(float));
  if (!A || !B || !C)
  {
    free(A);
    free(B);
    free(C);
    return -1.0;
  }

  for (size_t i = 0; i < sz; ++i)
  {
    A[i] = (float)((i % 13) * 0.1);
    B[i] = (float)((i % 7) * 0.2);
    C[i] = 0.0f;
  }

  double t0 = now_sec_local();
  uint64_t iters = 0;
  while (!(*g_stop) && now_sec_local() - t0 < s->duration_sec)
  {
    for (int t = 0; t < n; ++t)
      for (int i = 0; i < dim; ++i)
        for (int j = 0; j < dim; ++j)
        {
          float acc = 0.0f;
          for (int k = 0; k < dim; ++k)
            acc += A[(size_t)i * dim + k] * B[(size_t)k * dim + j];
          C[(size_t)i * dim + j] = acc;
        }
    iters++;
  }

  double elapsed = now_sec_local() - t0;
  free(A);
  free(B);
  free(C);
  if (elapsed <= 0.0)
    return -1.0;
  return (double)iters / elapsed;
}

size_t workload_parse_mem_bytes(const char *arg, int *was_clamped)
{
  if (was_clamped)
    *was_clamped = 0;
  if (!arg || !*arg)
    return 64u * 1024u * 1024u;
  char *end = NULL;
  long long v = strtoll(arg, &end, 10);
  if (end == arg || v <= 0)
    return 64u * 1024u * 1024u;

  size_t mul = 1;
  if (*end == 'K' || *end == 'k')
    mul = 1024u;
  else if (*end == 'M' || *end == 'm')
    mul = 1024u * 1024u;
  else if (*end == 'G' || *end == 'g')
    mul = 1024u * 1024u * 1024u;

  size_t out = (size_t)v * mul;
  if (out < 1024u * 1024u)
  {
    out = 1024u * 1024u;
    if (was_clamped)
      *was_clamped = 1;
  }
  if (out > 1024u * 1024u * 1024u)
  {
    out = 1024u * 1024u * 1024u;
    if (was_clamped)
      *was_clamped = 1;
  }
  return out;
}

void workload_run_memory_test(const Step *s,
                              double *read_mb_s,
                              double *write_mb_s,
                              double *copy_mb_s,
                              volatile sig_atomic_t *g_stop,
                              int *was_clamped)
{
  (void)g_stop;
  *read_mb_s = *write_mb_s = *copy_mb_s = -1.0;
  const size_t nbytes = workload_parse_mem_bytes(s->arg, was_clamped);
  const size_t n = nbytes / sizeof(uint64_t);

  uint64_t *a = NULL;
  uint64_t *b = NULL;
#ifdef _WIN32
  a = (uint64_t *)_aligned_malloc(nbytes, 64);
  b = (uint64_t *)_aligned_malloc(nbytes, 64);
#else
  if (posix_memalign((void **)&a, 64, nbytes) != 0)
    a = NULL;
  if (posix_memalign((void **)&b, 64, nbytes) != 0)
    b = NULL;
#endif
  if (!a || !b)
  {
#ifdef _WIN32
    if (a)
      _aligned_free(a);
    if (b)
      _aligned_free(b);
#else
    free(a);
    free(b);
#endif
    return;
  }

  for (size_t i = 0; i < n; ++i)
  {
    a[i] = (uint64_t)i;
    b[i] = 0;
  }

  volatile uint64_t sink = 0;
  double t0 = now_sec_local();
  for (size_t i = 0; i < n; ++i)
    sink += a[i];
  double t1 = now_sec_local();

  double t2 = now_sec_local();
  for (size_t i = 0; i < n; ++i)
    b[i] = (uint64_t)(i * 3u + 1u);
  double t3 = now_sec_local();

  double t4 = now_sec_local();
  memcpy(b, a, nbytes);
  double t5 = now_sec_local();

  (void)sink;

  const double mb = (double)nbytes / (1024.0 * 1024.0);
  if (t1 > t0)
    *read_mb_s = mb / (t1 - t0);
  if (t3 > t2)
    *write_mb_s = mb / (t3 - t2);
  if (t5 > t4)
    *copy_mb_s = mb / (t5 - t4);

#ifdef _WIN32
  _aligned_free(a);
  _aligned_free(b);
#else
  free(a);
  free(b);
#endif
}

void workload_run_jitter_test(const Step *s,
                              double *avg_us,
                              double *p50_us,
                              double *p95_us,
                              double *p99_us,
                              double *max_us,
                              uint64_t *over_500us,
                              uint64_t *over_1000us,
                              volatile sig_atomic_t *g_stop)
{
  *avg_us = *p50_us = *p95_us = *p99_us = *max_us = -1.0;
  *over_500us = *over_1000us = 0;

  int period_us = 1000;
  if (s->arg)
  {
    int v = atoi(s->arg);
    if (v >= 100 && v <= 20000)
      period_us = v;
  }

  const size_t cap = (size_t)(s->duration_sec * 1000000LL / period_us + 1024);
  Series vals;
  series_init(&vals, cap);

  struct timespec req, rem;
  req.tv_sec = 0;
  req.tv_nsec = period_us * 1000;

  double start = now_sec_local();
  double next = start;

  while (!(*g_stop) && now_sec_local() - start < s->duration_sec)
  {
    next += (double)period_us / 1e6;
    nanosleep(&req, &rem);
    double now = now_sec_local();
    double jitter = (now - next) * 1e6;
    if (jitter < 0.0)
      jitter = -jitter;
    series_push(&vals, jitter);
    if (jitter > 500.0)
      (*over_500us)++;
    if (jitter > 1000.0)
      (*over_1000us)++;
  }

  if (vals.n > 0)
  {
    StatsSummary st = stats_from_array(vals.v, vals.n);
    *avg_us = st.avg;
    *p50_us = st.median;
    *p95_us = st.p95;
    *p99_us = st.p99;
    *max_us = st.max;
  }

  series_free(&vals);
}