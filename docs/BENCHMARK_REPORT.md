# Laya Dedicated Per-Core Abstract Socket Benchmark Report

## System Architecture
- **Host**: gen5 (Debian 13 Trixie, Linux 6.12.107+deb13-amd64)
- **Hardware**: 8 vCPUs, 15 GiB RAM
- **Socket Scheme**: Linux Abstract Namespace (`@laya_server/<cpu>`)
- **Topology**: Dedicated worker thread and listen socket per CPU core
- **Total Requests**: 16000 across 16 concurrent threads

## Results Matrix

| Metric | Measured | Performance Gate | Status |
| :--- | :--- | :--- | :--- |
| **Success Rate** | 16000 / 16000 (100%) | 100% | PASS |
| **Socket Errors** | 0 | 0 | PASS |
| **p50 Latency** | 0.132573 ms (132.573 us) | < 25.0 ms | PASS |
| **p90 Latency** | 0.22251 ms (222.51 us) | < 30.0 ms | PASS |
| **p95 Latency** | 0.256658 ms (256.658 us) | < 35.0 ms | PASS |
| **p99 Latency** | 0.628579 ms (628.579 us) | < 50.0 ms | PASS |
| **Mean Latency** | 0.175611 ms (175.611 us) | < 25.0 ms | PASS |
| **Throughput** | 56692 req/sec | > 50 req/sec | PASS |
| **RSS Memory (Start)** | 4.90625 MB | < 500 MB | PASS |
| **RSS Memory (End)** | 4.90625 MB | < 500 MB | PASS |
| **Memory Delta** | 0 MB | Zero Leaks | PASS |

## Architectural Finding
The dedicated per-core abstract socket topology (`@laya_server/<cpu>`) maximizes CPU cache locality and completely eliminates inter-thread lock contention and dynamic CPU affinity switching overhead. Each client worker thread connects directly to the matching Laya worker on the same core.
