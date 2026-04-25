# SBC Benchmark Report (C)

- Scenario: **neural_host**
- Samples: **7**

## Aggregates

- Max temperature: -1.000 C
- Avg power: N/A

## Steps

| Step | Kind | CPU ops/s | NN inf/s | Storage MB/s | Ping p99 ms | Loss % |
|---|---|---:|---:|---:|---:|---:|
| idle_baseline | idle | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| nn_warmup | nn_inference | 0.000 | 185217.121 | 0.000 | 0.000 | 0.000 |
| storage_checkpoint | storage | 0.000 | 0.000 | 331.134 | 0.000 | 0.000 |
| network_probe | ping | 0.000 | 0.000 | 0.000 | -1.000 | -1.000 |
| nn_steady | nn_inference | 0.000 | 47673.451 | 0.000 | 0.000 | 0.000 |
