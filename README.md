# xdpflow

A minimal XDP/BPF packet counter that exposes its counters as Prometheus metrics.

The project consists of two parts:

- `xdp_count.c` — an XDP program that classifies IPv4 packets by transport protocol and increments a per-CPU counter.
- `obs_exporter_http.c` — a userspace exporter that reads the BPF map and serves the aggregated counters over HTTP in Prometheus text format.

## How it works

```
 NIC ──► XDP program ──► pkt_cnt_map (PERCPU_ARRAY)
                              │
                              ▼
                     obs_exporter_http :19090/metrics
                              │
                              ▼
                    Prometheus / curl / Grafana
```

`pkt_cnt_map` is a `BPF_MAP_TYPE_PERCPU_ARRAY` with 4 slots, one per protocol class:

| key | label      | meaning                      |
|-----|------------|------------------------------|
| 0   | `other`    | IPv4, not TCP/UDP/ICMP       |
| 1   | `tcp`      | TCP                          |
| 2   | `udp`      | UDP                          |
| 3   | `icmp`     | ICMP                         |

Every packet is returned with `XDP_PASS`, so the program only observes traffic and never drops it.

The exporter sums the per-CPU values before printing, so the numbers are machine-wide totals.

## Requirements

- Linux kernel with XDP support (4.18+ recommended)
- `clang` / `llvm` for compiling BPF programs
- `libbpf` (headers and library) plus `libelf` and `zlib`
- `bpftool` and `iproute2` for loading and attaching the program
- Root privileges (`CAP_BPF` / `CAP_NET_ADMIN`)

## Build

```bash
# BPF program
clang -O2 -g -target bpf -I/usr/include/$(uname -m)-linux-gnu -c xdp_count.c -o xdp_count.o

# Userspace exporter
gcc -O2 -g obs_exporter_http.c -o obs_exporter_http -lbpf -lelf -lz
```

## Run

1. Load and pin the program, then attach it to an interface:

```bash
bpftool prog load xdp_count.o /sys/fs/bpf/xdp_count_prog type xdp
ip link set dev eth0 xdp pinned /sys/fs/bpf/xdp_count_prog
```

2. Pin the map (optional, used as a fallback lookup path):

```bash
bpftool map pin name pkt_cnt_map /sys/fs/bpf/xdp_count
```

3. Start the exporter:

```bash
sudo ./obs_exporter_http
```

4. Scrape it:

```bash
curl http://localhost:19090/metrics
```

Expected output:

```
# HELP ebpf_packets_total Total packets counted by XDP program
# TYPE ebpf_packets_total counter
ebpf_packets_total{protocol="other"} 128
ebpf_packets_total{protocol="tcp"} 40921
ebpf_packets_total{protocol="udp"} 1002
ebpf_packets_total{protocol="icmp"} 7
```

## Map lookup

The exporter tries two ways to reach the map, in order:

1. Scan all loaded BPF maps by name (`pkt_cnt_map`) and verify the layout: type `PERCPU_ARRAY`, 4 entries, 8-byte values. If a map has the same name but a different layout, a warning is printed and the search continues.
2. Fall back to the pinned path `/sys/fs/bpf/xdp_count`.

The first method means the exporter can attach to a program that was loaded without pinning the map.

## Prometheus

Add a scrape target:

```yaml
scrape_configs:
  - job_name: 'obs_exporter'
    static_configs:
      - targets: ['localhost:19090']
```

A working example lives in `../prometheus/prometheus.yml`.

## Cleanup

```bash
ip link set dev eth0 xdp off
rm -f /sys/fs/bpf/xdp_count_prog /sys/fs/bpf/xdp_count
```

## Limitations

- IPv4 only; IPv6 and VLAN-tagged frames are ignored.
- Only TCP/UDP/ICMP are broken out; everything else is lumped into `other`.
- The exporter is single-threaded and handles one request at a time.
- Every request rebuilds the metrics payload from scratch — fine for a handful of counters, not for large maps.
- No IPv6 listener and no TLS; keep it on a trusted network.

## Files

```
xdpflow/
├── xdp_count.c            # XDP program
├── obs_exporter_http.c    # Prometheus exporter
├── READEME.md             # this file (English)
└── README.zh-CN.md        # Chinese version
```
