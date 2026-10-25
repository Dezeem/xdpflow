// obs_exporter_http.c
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#define MAX_ENTRIES 4
#define PORT 19090
#define MAP_NAME "pkt_cnt_map"
#define MAP_PIN_PATH "/sys/fs/bpf/xdp_count"

// Protocol name
const char *proto_name[MAX_ENTRIES] = {
    "other", "tcp", "udp", "icmp"
};

int map_fd;

//==============================
// To find map by name
//==============================
static int find_map_fd_by_name(const char *name) {
    __u32 id = 0;

    while (bpf_map_get_next_id(id, &id) == 0) {
        struct bpf_map_info info;
        __u32 len = sizeof(info);
        int fd = bpf_map_get_fd_by_id(id);
        if (fd < 0) {
            continue;
        }

        memset(&info, 0, sizeof(info));
        if (bpf_obj_get_info_by_fd(fd, &info, &len) == 0 &&
            strncmp((const char *)info.name, name, BPF_OBJ_NAME_LEN) == 0) {
            // check map type, max entries, value size
            if (info.type == BPF_MAP_TYPE_PERCPU_ARRAY &&
                info.max_entries == MAX_ENTRIES &&
                info.value_size == sizeof(__u64)) {
                return fd;
            }
            fprintf(stderr,
                "Found map with the same name \"%s\" but layout mismatch (type=%u entries=%u value_size=%u)\n",
                name, info.type, info.max_entries, info.value_size);
        }

        close(fd);
    }

    return -1;
}

//==============================
// build metrics
//==============================
int build_metrics(char *buf, size_t size) {
    int offset = 0;

    // Prometheus Header
    offset += snprintf(buf + offset, size - offset,
        "# HELP ebpf_packets_total Total packets counted by XDP program\n"
        "# TYPE ebpf_packets_total counter\n");

    // get ncpu
    int ncpu = libbpf_num_possible_cpus();
    if (ncpu <= 0) ncpu = 16;

    for (int i = 0; i < MAX_ENTRIES; i++) {
        __u32 key = i;
        __u64 total_val = 0;

        __u64 *per_cpu_vals = calloc(ncpu, sizeof(__u64));
        if (!per_cpu_vals) continue;

        int ret = bpf_map_lookup_elem(map_fd, &key, per_cpu_vals);
        if (ret == 0) {
            for (int c = 0; c < ncpu; c++) {
                total_val += per_cpu_vals[c];
            }
        }

        free(per_cpu_vals);

        offset += snprintf(buf + offset, size - offset,
            "ebpf_packets_total{protocol=\"%s\"} %llu\n",
            proto_name[i], total_val);
    }

    return offset;
}

//==============================
// HTTP Server
//==============================
void handle_client(int client_fd) {
    char metrics[8192];
    int len = build_metrics(metrics, sizeof(metrics));

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; version=0.0.4\r\n"
        "Content-Length: %d\r\n"
        "\r\n", len);

    write(client_fd, header, strlen(header));
    write(client_fd, metrics, len);
}

int main() {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== eBPF HTTP Exporter ===\n");

    // first try to find map by name
    map_fd = find_map_fd_by_name(MAP_NAME);
    if (map_fd >= 0) {
        printf("Found BPF map \"%s\" by name (FD=%d)\n", MAP_NAME, map_fd);
    } else {
        map_fd = bpf_obj_get(MAP_PIN_PATH);
        if (map_fd < 0) {
            fprintf(stderr, "Failed to open BPF map (name=%s, pin=%s): %s\n",
                    MAP_NAME, MAP_PIN_PATH, strerror(errno));
            return 1;
        }
        printf("Opened BPF map via pin path %s (FD=%d)\n", MAP_PIN_PATH, map_fd);
    }

    // HTTP Server
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return 1;
    }
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT)
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to bind %d (port may already be in use): %s\n", PORT, strerror(errno));
        return 1;
    }
    if (listen(server_fd, 5) < 0) {
        fprintf(stderr, "Failed to listen: %s\n", strerror(errno));
        return 1;
    }

    printf("Exporter listening on: http://0.0.0.0:%d/metrics\n", PORT);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "Failed to accept: %s\n", strerror(errno));
            break;
        }
        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
