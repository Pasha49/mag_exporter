#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 9100
static char body[16384];
static char header[512];

void collect_metrics(char* b, size_t max) {
    int off = 0;
    char line[512];
    FILE* f;

    // 1. Load Average
    f = fopen("/proc/loadavg", "r");
    if (f) {
        double l1, l5, l15;
        if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            off += snprintf(b + off, max - off, "node_load1 %.2f\nnode_load5 %.2f\nnode_load15 %.2f\n", l1, l5, l15);
        }
        fclose(f);
    }

    // 2. Global CPU Stats (из /proc/stat)
    f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long u, n, s, i, iw, irq, sirq;
        if (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &iw, &irq, &sirq) >= 4) {
                off += snprintf(b + off, max - off, 
                    "node_cpu_seconds_total{mode=\"user\"} %.2f\n"
                    "node_cpu_seconds_total{mode=\"system\"} %.2f\n"
                    "node_cpu_seconds_total{mode=\"idle\"} %.2f\n"
                    "node_cpu_seconds_total{mode=\"iowait\"} %.2f\n"
                    "node_cpu_seconds_total{mode=\"irq\"} %.2f\n",
                    (double)u/100.0, (double)s/100.0, (double)i/100.0, (double)iw/100.0, (double)irq/100.0);
            }
        }
        while(fgets(line, sizeof(line), f)) {
            unsigned long long val;
            if (sscanf(line, "ctxt %llu", &val)) off += snprintf(b + off, max - off, "node_context_switches_total %llu\n", val);
            if (sscanf(line, "intr %llu", &val)) off += snprintf(b + off, max - off, "node_intr_total %llu\n", val);
        }
        fclose(f);
    }

    // 3. Memory Stats (из /proc/meminfo)
    f = fopen("/proc/meminfo", "r");
    if (f) {
        unsigned long val;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %lu", &val)) off += snprintf(b + off, max - off, "node_memory_MemTotal_bytes %lu\n", val * 1024);
            if (sscanf(line, "MemFree: %lu", &val)) off += snprintf(b + off, max - off, "node_memory_MemFree_bytes %lu\n", val * 1024);
            if (sscanf(line, "Buffers: %lu", &val)) off += snprintf(b + off, max - off, "node_memory_Buffers_bytes %lu\n", val * 1024);
            if (sscanf(line, "Cached: %lu", &val)) off += snprintf(b + off, max - off, "node_memory_Cached_bytes %lu\n", val * 1024);
        }
        fclose(f);
    }

    // 4. Uptime
    f = fopen("/proc/uptime", "r");
    if (f) {
        double up;
        if (fscanf(f, "%lf", &up) == 1) off += snprintf(b + off, max - off, "node_uptime_seconds %.2f\n", up);
        fclose(f);
    }

    // 5. Network (eth0)
    f = fopen("/proc/net/dev", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "eth0:")) {
                char *p = strchr(line, ':') + 1;
                unsigned long long rx, tx, rx_p, tx_p, rx_err, tx_err;
                // Парсим: bytes, packets, errs...
                if (sscanf(p, "%llu %llu %llu %*u %*u %*u %*u %*u %llu %llu %llu", &rx, &rx_p, &rx_err, &tx, &tx_p, &tx_err) >= 4) {
                    off += snprintf(b + off, max - off, 
                        "node_network_receive_bytes_total %llu\n"
                        "node_network_receive_packets_total %llu\n"
                        "node_network_receive_errors_total %llu\n"
                        "node_network_transmit_bytes_total %llu\n"
                        "node_network_transmit_packets_total %llu\n", rx, rx_p, rx_err, tx, tx_p);
                }
            }
        }
        fclose(f);
    }

    // 6. UDP Multicast
    f = fopen("/proc/net/udp", "r");
    if (f) {
        int udp_cnt = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned int addr;
            if (sscanf(line, "%*d: %x:", &addr) == 1) {
                if ((unsigned char)(addr & 0xFF) >= 0xE0) udp_cnt++;
            }
        }
        off += snprintf(b + off, max - off, "mag250_udp_streams_total %d\n", udp_cnt);
        fclose(f);
    }
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    listen(fd, 2);

    printf("Rich Metrics Exporter started on port %d\n", PORT);

    while (1) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) continue;

        char req[512]; read(client, req, sizeof(req));
        
        memset(body, 0, sizeof(body));
        collect_metrics(body, sizeof(body));

        int hlen = snprintf(header, sizeof(header), 
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", 
            strlen(body));

        write(client, header, hlen);
        write(client, body, strlen(body));
        close(client);
    }
    return 0;
}
