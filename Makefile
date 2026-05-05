CC=gcc
CFLAGS=-std=c11 -O2 -pthread
LDFLAGS=-lm
SRC=sbc_bench_v4.c sbc_bench_scenarios.c sbc_bench_workloads.c \
    sbc_bench_storage.c sbc_bench_network.c sbc_bench_metrics.c \
    sbc_bench_report.c sbc_bench_coordinator.c sbc_bench_noise.c \
    sbc_bench_telemetry.c

all: sbc_bench_v4

sbc_bench_v4: $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $@

smoke:
	$(CC) -std=c11 -O2 tests/metrics_smoke.c sbc_bench_metrics.c -lm -o metrics_smoke
	./metrics_smoke

clean:
	rm -f sbc_bench_v4 metrics_smoke