# xdpflow

一个极简的 XDP/BPF 数据包计数器，并把计数结果以 Prometheus 指标的形式暴露出来。

项目由两部分组成：

- `xdp_count.c` —— XDP 程序，按传输层协议对 IPv4 报文分类，并累加 per-CPU 计数器。
- `obs_exporter_http.c` —— 用户态 exporter，读取 BPF map，把聚合后的计数通过 HTTP 以 Prometheus 文本格式输出。

## 工作原理

```
 网卡 ──► XDP 程序 ──► pkt_cnt_map (PERCPU_ARRAY)
                            │
                            ▼
                   obs_exporter_http :19090/metrics
                            │
                            ▼
                  Prometheus / curl / Grafana
```

`pkt_cnt_map` 是一个 `BPF_MAP_TYPE_PERCPU_ARRAY`，共 4 个槽位，每个槽位对应一类协议：

| key | label   | 含义                   |
|-----|---------|------------------------|
| 0   | `other` | IPv4，且非 TCP/UDP/ICMP |
| 1   | `tcp`   | TCP                    |
| 2   | `udp`   | UDP                    |
| 3   | `icmp`  | ICMP                   |

所有报文统一返回 `XDP_PASS`，因此该程序只观察流量，不会丢包。

exporter 在输出前会把各 CPU 的计数累加，得到整机总量。

## 环境要求

- 支持 XDP 的 Linux 内核（建议 4.18 及以上）
- `clang` / `llvm`：用于编译 BPF 程序
- `libbpf`（头文件与库）、`libelf`、`zlib`
- `bpftool` 与 `iproute2`：用于加载和挂载程序
- root 权限（`CAP_BPF` / `CAP_NET_ADMIN`）

## 编译

```bash
# BPF 程序
clang -O2 -g -target bpf -I/usr/include/$(uname -m)-linux-gnu -c xdp_count.c -o xdp_count.o

# 用户态 exporter
gcc -O2 -g obs_exporter_http.c -o obs_exporter_http -lbpf -lelf -lz
```

## 运行

1. 加载并 pin 程序，然后挂载到网卡：

```bash
bpftool prog load xdp_count.o /sys/fs/bpf/xdp_count_prog type xdp
ip link set dev eth0 xdp pinned /sys/fs/bpf/xdp_count_prog
```

2. 挂载（pin）map（可选，作为兜底查找路径）：

```bash
bpftool map pin name pkt_cnt_map /sys/fs/bpf/xdp_count
```

3. 启动 exporter：

```bash
sudo ./obs_exporter_http
```

4. 抓取指标：

```bash
curl http://localhost:19090/metrics
```

输出示例：

```
# HELP ebpf_packets_total Total packets counted by XDP program
# TYPE ebpf_packets_total counter
ebpf_packets_total{protocol="other"} 128
ebpf_packets_total{protocol="tcp"} 40921
ebpf_packets_total{protocol="udp"} 1002
ebpf_packets_total{protocol="icmp"} 7
```

## map 查找方式

exporter 按以下顺序尝试连接 map：

1. 按名字 `pkt_cnt_map` 遍历所有已加载的 BPF map，并校验结构：类型为 `PERCPU_ARRAY`、4 个条目、value 长度 8 字节。若发现同名但结构不符的 map，会打印告警并继续查找。
2. 若上一步失败，则回退到 pin 路径 `/sys/fs/bpf/xdp_count`。

第一种方式意味着：即使加载程序时没有 pin map，exporter 也能连上。

## 接入 Prometheus

添加抓取目标：

```yaml
scrape_configs:
  - job_name: 'obs_exporter'
    static_configs:
      - targets: ['localhost:19090']
```

可直接使用的配置示例见 `../prometheus/prometheus.yml`。

## 清理

```bash
ip link set dev eth0 xdp off
rm -f /sys/fs/bpf/xdp_count_prog /sys/fs/bpf/xdp_count
```

## 已知限制

- 仅支持 IPv4，IPv6 与带 VLAN tag 的报文会被忽略。
- 只细分 TCP/UDP/ICMP，其余协议统一计入 `other`。
- exporter 是单线程的，同一时刻只处理一个请求。
- 每次请求都会重新构建完整的指标内容：计数器很少时没问题，map 很大时就不合适了。
- 没有 IPv6 监听，也没有 TLS，请只在可信网络中使用。

## 文件说明

```
xdpflow/
├── xdp_count.c            # XDP 程序
├── obs_exporter_http.c    # Prometheus exporter
├── READEME.md             # 英文说明
└── README.zh-CN.md        # 本文件（中文说明）
```
