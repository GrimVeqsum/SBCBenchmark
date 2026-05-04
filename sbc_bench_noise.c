#define _POSIX_C_SOURCE 200809L
#include "sbc_bench_noise.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#define fsync _commit
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct NoiseContext
{
  NoiseMode mode;
  volatile sig_atomic_t stop;

  pthread_t *cpu_threads;
  int cpu_threads_n;

  pthread_t io_thread;
  int io_enabled;

  char io_dir[PATH_MAX];
};

typedef struct
{
  volatile sig_atomic_t *stop;
} CpuNoiseArg;

typedef struct
{
  volatile sig_atomic_t *stop;
  char io_dir[PATH_MAX];
} IoNoiseArg;

static void *cpu_noise_worker(void *arg)
{
  CpuNoiseArg *a = (CpuNoiseArg *)arg;
  uint32_t x = 1u;
  while (!*(a->stop))
  {
    for (int i = 0; i < 30000; ++i)
      x = x * 1664525u + 1013904223u;
    (void)x;
  }
  return NULL;
}

static void *io_noise_worker(void *arg)
{
  IoNoiseArg *a = (IoNoiseArg *)arg;
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/noise_io.bin", a->io_dir[0] ? a->io_dir : ".");

  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0)
    return NULL;

  const size_t bs = 1024 * 1024;
  char *buf = (char *)malloc(bs);
  if (!buf)
  {
    close(fd);
    return NULL;
  }
  memset(buf, 0xA5, bs);

  while (!*(a->stop))
  {
    ssize_t wr = write(fd, buf, bs);
    if (wr < 0)
    {
      if (errno == EINTR)
        continue;
      break;
    }
    fsync(fd);
  }

  free(buf);
  close(fd);
  unlink(path);
  return NULL;
}

NoiseContext *noise_start(NoiseMode mode, int cpu_threads, const char *io_dir)
{
  if (mode == NOISE_NONE)
    return NULL;

  NoiseContext *ctx = (NoiseContext *)calloc(1, sizeof(NoiseContext));
  if (!ctx)
    return NULL;

  ctx->mode = mode;
  ctx->stop = 0;
  if (io_dir)
    snprintf(ctx->io_dir, sizeof(ctx->io_dir), "%s", io_dir);
  else
    snprintf(ctx->io_dir, sizeof(ctx->io_dir), ".");

  const int need_cpu = (mode == NOISE_CPU || mode == NOISE_COMBINED);
  const int need_io = (mode == NOISE_IO || mode == NOISE_COMBINED);

  if (need_cpu)
  {
    if (cpu_threads <= 0)
      cpu_threads = 1;
    ctx->cpu_threads_n = cpu_threads;
    ctx->cpu_threads = (pthread_t *)calloc((size_t)cpu_threads, sizeof(pthread_t));
    if (!ctx->cpu_threads)
    {
      free(ctx);
      return NULL;
    }

    for (int i = 0; i < cpu_threads; ++i)
    {
      CpuNoiseArg *a = (CpuNoiseArg *)malloc(sizeof(CpuNoiseArg));
      if (!a)
        continue;
      a->stop = &ctx->stop;
      if (pthread_create(&ctx->cpu_threads[i], NULL, cpu_noise_worker, a) != 0)
      {
        free(a);
      }
    }
  }

  if (need_io)
  {
    IoNoiseArg *a = (IoNoiseArg *)malloc(sizeof(IoNoiseArg));
    if (a)
    {
      a->stop = &ctx->stop;
      snprintf(a->io_dir, sizeof(a->io_dir), "%s", ctx->io_dir);
      if (pthread_create(&ctx->io_thread, NULL, io_noise_worker, a) == 0)
        ctx->io_enabled = 1;
      else
        free(a);
    }
  }

  return ctx;
}

void noise_stop(NoiseContext *ctx)
{
  if (!ctx)
    return;

  ctx->stop = 1;

  if (ctx->cpu_threads)
  {
    for (int i = 0; i < ctx->cpu_threads_n; ++i)
    {
      if (ctx->cpu_threads[i])
        pthread_join(ctx->cpu_threads[i], NULL);
    }
    free(ctx->cpu_threads);
  }

  if (ctx->io_enabled)
    pthread_join(ctx->io_thread, NULL);

  free(ctx);
}