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

/* ... остальная часть файла без изменений ... */