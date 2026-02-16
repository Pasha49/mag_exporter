#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 9100
#define BUFFER_SIZE 32768

const char* target_procs[] = {
    "AUD[0].PESTask", "AUD[0].DecTask", "AUD[0].MixTask", 
    "STVID[0].H264Pa", "STVID[0].Produc", "STVID[0].Displa", 
    "MAG250_ControlT", NULL
};

void gather_metrics(char* out, size_t max_len) {
    int offset = 0;
    char line[256];

    // 1. Load Average
    FILE* fp = fopen("/proc/loadavg", "r");
    if (fp) {
        double l1, l5, l15;
        if (fscanf(fp, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            offset += snprintf(out + offset, max_len - offset, "node_load1 %.2f\nnode_load5 %.2f\nnode_load15 %.2f\n", l1, l5, l15);
        }
        fclose(fp);
    }

    // 2. Network (eth0)
    fp = fopen("/proc/net/dev", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "eth0:")) {
                char *ptr = strchr(line, ':') + 1;
                unsigned long long rx_bytes = 0, tx_bytes = 0;
                sscanf(ptr, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx_bytes, &tx_bytes);
                offset += snprintf(out + offset, max_len - offset, "node_network_receive_bytes_total{device=\"eth0\"} %llu\n", rx_bytes);
                offset += snprintf(out + offset, max_len - offset, "node_network_transmit_bytes_total{device=\"eth0\"} %llu\n", tx_bytes);
                break;
            }
        }
        fclose(fp);
    }

    // 3. UDP Streams (netstat)
    fp = popen("netstat -uln 2>/dev/null", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "udp ", 4) == 0) {
                char p1[32], p2[32], p3[32], p4[128];
                if (sscanf(line, "%s %s %s %s", p1, p2, p3, p4) >= 4) {
                    if (strncmp(p4, "224.", 4) == 0 || strncmp(p4, "239.", 4) == 0) {
                        offset += snprintf(out + offset, max_len - offset, "mag250_udp_stream_active{local_address=\"%s\"} 1\n", p4);
                    }
                }
            }
        }
        pclose(fp);
    }

    // 4. Processes CPU
    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (isdigit(ent->d_name[0])) {
                char path[256];
                snprintf(path, sizeof(path), "/proc/%s/stat", ent->d_name);
                FILE* sfp = fopen(path, "r");
                if (sfp) {
                    if (fgets(line, sizeof(line), sfp)) {
                        char* start = strchr(line, '(');
                        char* end = strrchr(line, ')');
                        if (start && end && end > start) {
                            *end = '\0';
                            char* name = start + 1;
                            int match = 0;
                            for (int i = 0; target_procs[i] != NULL; i++) {
                                if (strcmp(name, target_procs[i]) == 0) {
                                    match = 1;
                                    break;
                                }
                            }
                            if (match) {
                                char* cur = end + 2;
                                unsigned long utime = 0, stime = 0;
                                int field = 3;
                                char* token = strtok(cur, " ");
                                while (token != NULL) {
                                    if (field == 14) utime = strtoul(token, NULL, 10);
                                    if (field == 15) {
                                        stime = strtoul(token, NULL, 10);
                                        break;
                                    }
                                    field++;
                                    token = strtok(NULL, " ");
                                }
                                offset += snprintf(out + offset, max_len - offset, "mag250_process_cpu_ticks_total{process=\"%s\",mode=\"user\"} %lu\n", name, utime);
                                offset += snprintf(out + offset, max_len - offset, "mag250_process_cpu_ticks_total{process=\"%s\",mode=\"system\"} %lu\n", name, stime);
                            }
                        }
                    }
                    fclose(sfp);
                }
            }
        }
        closedir(dir);
    }
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[2048] = {0};
    char *response_body = malloc(BUFFER_SIZE);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) return 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) return 1;
    if (listen(server_fd, 5) < 0) return 1;

    printf("MAG250 Exporter started on http://0.0.0.0:%d/metrics\n", PORT);

    while (1) {
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) continue;
        
        int r = read(new_socket, buffer, sizeof(buffer)-1);
        if (r > 0) {
            buffer[r] = '\0';
            if (strncmp(buffer, "GET /metrics", 12) == 0) {
                gather_metrics(response_body, BUFFER_SIZE);
                
                char header[256];
                snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", strlen(response_body));
                
                write(new_socket, header, strlen(header));
                write(new_socket, response_body, strlen(response_body));
            } else {
                char *not_found = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
                write(new_socket, not_found, strlen(not_found));
            }
        }
        close(new_socket);
    }
    free(response_body);
    return 0;
}
