#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>

// map definition.
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 4);
    __type(key, __u32);
    __type(value, __u64);
} pkt_cnt_map SEC(".maps");

SEC("xdp")
    int xdp_count_prog(struct xdp_md *ctx) {
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    // Ethernet header
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end) {
        return XDP_PASS;
    }

    // Check if it's IP packet
    if (eth->h_proto != __constant_htons(ETH_P_IP)) {
        return XDP_PASS;
    }

    // IP header
    struct iphdr *ip = (struct iphdr *)(eth + 1);
    if ((void *)(ip + 1) > data_end) {
        return XDP_PASS;
    }

    __u32 key = 0;

    // Classify by protocol
    switch (ip->protocol) {
        case IPPROTO_TCP:
            key = 1;
            break;
        case IPPROTO_UDP:
            key = 2;
            break;
        case IPPROTO_ICMP:
            key = 3;
            break;
        default:
            key = 0; // Other protocols
            break;
    }

    // Update counter - use correct map reference
    __u64 *value = bpf_map_lookup_elem(&pkt_cnt_map, &key);
    if (value) {
        *value += 1;
    }

    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";