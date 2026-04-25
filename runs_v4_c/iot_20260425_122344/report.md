# SBC Benchmark Report (C)

- Scenario: **iot**
- Samples: **5**

## Aggregates

- Max temperature: -1.000 C
- Avg power: N/A

## Steps

| Step | Kind | CPU ops/s | NN inf/s | Storage MB/s | Ping p99 ms | Loss % |
|---|---|---:|---:|---:|---:|---:|
| idle_baseline | idle | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| sensor_batch_compute | cpu_burn | 59047952999404.930 | 0.000 | 0.000 | 0.000 | 0.000 |
| persist_batch | storage | 0.000 | 0.000 | 215.833 | 0.000 | 0.000 |
| uplink_health | ping | 0.000 | 0.000 | 0.000 | -1.000 | -1.000 |
| sleep_window | idle | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
