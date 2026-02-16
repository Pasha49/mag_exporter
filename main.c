#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#define PORT 9100
#define BUFFER_SIZE 32768

int debug = 0;

const char* target_procs[] = {
    "AUD[0].PESTask", "AUD[0].DecTask", "AUD[0].MixTask", 
    "STVID[0].H264Pa", "STVID[0].Produc", "STVID[0].Displa", 
    "MAG250_ControlT", NULL
};

void log_debug(const char* msg) {
    if (debug) {
        printf("[DEBUG] %s\n", msg);
        fflush(stdout);
    }
}

void gather_metrics(char* out, size_t max_len) {
    int offset = 0;
    char line[512];

    log_debug("Starting metrics collection...");

    // 1. Load Average
    FILE* fp = fopen("/proc/loadavg", "r");
    if (fp) {
        double l1, l5, l15;
        if (fscanf(fp, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            offset += snprintf(out + offset, max_len - offset, "node_load1 %.2f\nnode_load5 %.2f\nnode_load15 %.2f\n", l1, l5, l15);
        }
        fclose(fp);
    }
    log_debug("LoadAvg collected");

    // 2. Network (eth0)
    fp = fopen("/proc/net/dev", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "eth0:")) {
                char *ptr = strchr(line, ':') + 1;
                unsigned long long rx_bytes = 0, tx_bytes = 0;
                if (sscanf(ptr, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx_bytes, &tx_bytes) >= 1) {
                    offset += snprintf(out + offset, max_len - offset, "node_network_receive_bytes_total{device=\"eth0\"} %llu\n", rx_bytes);
                    offset += snprintf(out + offset, max_len - offset, "node_network_transmit_bytes_total{device=\"eth0\"} %llu\n", tx_bytes);
                }
                break;
            }
        }
        fclose(fp);
    }
    log_debug("Network metrics collected");

    // 3. UDP Streams
    fp = fopen("/proc/net/udp", "r");
    if (fp) {
        int stream_count = 0;
        fgets(line, sizeof(line), fp); 
        while (fgets(line, sizeof(line), fp)) {
            unsigned int local_addr;
            if (sscanf(line, "%*d: %x:", &local_addr) == 1) {
                unsigned char first_byte = (unsigned char)(local_addr & 0xFF);
                if (first_byte >= 0xE0 && first_byte <= 0xEF) {
                    stream_count++;
                }
            }
        }
        offset += snprintf(out + offset, max_len - offset, "mag250_udp_streams_total %d\n", stream_count);
        fclose(fp);
    }
    log_debug("UDP metrics collected");

    // 4. Processes CPU
    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (isdigit(ent->d_name[0])) {
                char stat_path[256];
                snprintf(stat_path, sizeof(stat_path), "/proc/%s/stat", ent->d_name);
                FILE* sfp = fopen(stat_path, "r");
                if (sfp) {
                    if (fgets(line, sizeof(line), sfp)) {
                        char* start = strchr(line, '(');
                        char* end = strrchr(line, ')');
                        if (start && end && end > start) {
                            *end = '\0';
                            char* name = start + 1;
                            for (int i = 0; target_procs[i] != NULL; i++) {
                                if (strcmp(name, target_procs[i]) == 0) {
                                    unsigned long utime = 0, stime = 0;
                                    if (sscanf(end + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) == 2) {
                                        offset += snprintf(out + offset, max_len - offset, "mag250_process_cpu_ticks_total{process=\"%s\",mode=\"user\"} %lu\n", name, utime);
                                        offset += snprintf(out + offset, max_len - offset, "mag250_process_cpu_ticks_total{process=\"%s\",mode=\"system\"} %lu\n", name, stime);
                                    }
                                }
                            }
                        }
                    }
                    fclose(sfp);
                }
            }
        }
        closedir(dir);
    }
    log_debug("Process metrics collected");
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        debug = 1;
        printf("Debug mode enabled\n");
    }

    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char *response_body = malloc(BUFFER_SIZE);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        return 1;
    }
    
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        return 1;
    }
    if (listen(server_fd, 5) < 0) {
        perror("listen failed");
        return 1;
    }

    printf("MAG250 Exporter started on port %d\n", PORT);

    while (1) {
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (new_socket < 0) {
            if (errno == EINTR) continue;
            perror("accept failed");
            continue;
        }

        log_debug("New connection accepted");

        char req_buf[1024] = {0};
        read(new_socket, req_buf, sizeof(req_buf)-1);
        
        memset(response_body, 0, BUFFER_SIZE);
        gather_metrics(response_body, BUFFER_SIZE);
        
        char header[512];
        int header_len = snprintf(header, sizeof(header), 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; version=0.0.4\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n", 
            strlen(response_body));
        
        write(new_socket, header, header_len);
        write(new_socket, response_body, strlen(response_body));
        
        log_debug("Response sent and closing socket");
        close(new_socket);
    }

    free(response_body);
    return 0;
}
