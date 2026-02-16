#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

#define PORT 9100
#define METRICS_FILE "/dev/shm/mag_metrics.prom"
#define TEMP_FILE "/dev/shm/mag_metrics.tmp"

static char buffer[16384];
static char tmp_line[1024];

const char* target_names[] = {
    "MAG250_ControlT", "AUD[0].PESTask", "AUD[0].DecTask", "AUD[0].MixTask", 
    "STVID[0].H264Pa", "STVID[0].Produc", "STVID[0].Displa", NULL
};


void fast_read(const char* path, char* dest, size_t max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { dest[0] = 0; return; }
    int n = read(fd, dest, max - 1);
    if (n > 0) dest[n] = 0; else dest[0] = 0;
    close(fd);
}

void collector_loop() {
    while (1) {
        int fd = open(TEMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { sleep(5); continue; }

        int len = 0;

        // 1. Time & Uptime
        fast_read("/proc/uptime", tmp_line, sizeof(tmp_line));
        double up; if(sscanf(tmp_line, "%lf", &up)) 
            len += snprintf(buffer+len, sizeof(buffer)-len, "node_uptime_seconds %.2f\n", up);
        
        len += snprintf(buffer+len, sizeof(buffer)-len, "mag_exporter_last_update_timestamp %ld\n", time(NULL));

        // 2. Load Average
        fast_read("/proc/loadavg", tmp_line, sizeof(tmp_line));
        double l1, l5, l15;
        if (sscanf(tmp_line, "%lf %lf %lf", &l1, &l5, &l15) == 3)
            len += snprintf(buffer+len, sizeof(buffer)-len, "node_load1 %.2f\nnode_load5 %.2f\nnode_load15 %.2f\n", l1, l5, l15);

        // 3. Network
        fast_read("/proc/net/dev", tmp_line, sizeof(tmp_line));
        char net_buf[4096];
        fast_read("/proc/net/dev", net_buf, sizeof(net_buf));
        char* p = strstr(net_buf, "eth0:");
        if (p) {
            unsigned long long rx, tx;
            sscanf(p + 5, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rx, &tx);
            len += snprintf(buffer+len, sizeof(buffer)-len, "node_network_receive_bytes_total %llu\nnode_network_transmit_bytes_total %llu\n", rx, tx);
        }

        // 4. UDP Streams
        char udp_buf[8192];
        fast_read("/proc/net/udp", udp_buf, sizeof(udp_buf));
        int udp_cnt = 0;
        p = udp_buf;
        while ((p = strchr(p, '\n')) != NULL) {
            p++; unsigned int addr;
            if (sscanf(p, "%*d: %x:", &addr) == 1) {
                if ((unsigned char)(addr & 0xFF) >= 0xE0) udp_cnt++;
            }
        }
        len += snprintf(buffer+len, sizeof(buffer)-len, "mag250_udp_streams_total %d\n", udp_cnt);

        // 5. PROCESSES
        DIR* dir = opendir("/proc");
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != NULL) {
                if (isdigit(ent->d_name[0])) {
                    int pid = atoi(ent->d_name);
                    
                    char path[64];
                    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
                    char cmd[256];
                    fast_read(path, cmd, sizeof(cmd));
                    
                    char* proc_name = NULL;
                    
                    for (int i = 0; target_names[i]; i++) {
                        if (strstr(cmd, target_names[i])) {
                            proc_name = (char*)target_names[i];
                            break;
                        }
                    }

                    if (!proc_name) {
                        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
                        int sfd = open(path, O_RDONLY | O_NONBLOCK); // Попытка неблокирующего открытия
                        if (sfd >= 0) {
                            char stat_buf[512];
                            int n = read(sfd, stat_buf, sizeof(stat_buf)-1);
                            close(sfd);
                            if (n > 0) {
                                stat_buf[n] = 0;
                                char* op = strchr(stat_buf, '(');
                                char* cp = strrchr(stat_buf, ')');
                                if (op && cp) {
                                    *cp = 0;
                                    char* comm = op + 1;
                                    for (int i = 0; target_names[i]; i++) {
                                        if (strcmp(comm, target_names[i]) == 0) {
                                            proc_name = (char*)target_names[i];
                                            char state; unsigned long ut, st;
                                            if (sscanf(cp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st) >= 3) {
                                                 len += snprintf(buffer+len, sizeof(buffer)-len, 
                                                    "mag250_process_cpu_ticks{name=\"%s\",mode=\"user\"} %lu\n"
                                                    "mag250_process_cpu_ticks{name=\"%s\",mode=\"system\"} %lu\n"
                                                    "mag250_process_blocked{name=\"%s\"} %d\n", 
                                                    proc_name, ut, proc_name, st, proc_name, (state == 'D'));
                                            }
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        // всё равно надо читать stat для CPU
                        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
                        fast_read(path, tmp_line, sizeof(tmp_line));
                        char* cp = strrchr(tmp_line, ')');
                        if (cp) {
                             char state; unsigned long ut, st;
                             sscanf(cp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st);
                             len += snprintf(buffer+len, sizeof(buffer)-len, 
                                "mag250_process_cpu_ticks{name=\"%s\",mode=\"user\"} %lu\n"
                                "mag250_process_cpu_ticks{name=\"%s\",mode=\"system\"} %lu\n"
                                "mag250_process_blocked{name=\"%s\"} %d\n", 
                                proc_name, ut, proc_name, st, proc_name, (state == 'D'));
                        }
                    }
                }
            }
            closedir(dir);
        }

        write(fd, buffer, len);
        close(fd);
        rename(TEMP_FILE, METRICS_FILE);
        
        sleep(5);
    }
}


int main() {
    pid_t pid = fork();
    if (pid == 0) {
        collector_loop();
        exit(0);
    }
    
    if (pid < 0) {
        perror("Fork failed");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    listen(server_fd, 10);

    printf("Split-Architecture Exporter started on port %d. PID: %d, Collector PID: %d\n", PORT, getpid(), pid);

    while (1) {
        int client = accept(server_fd, NULL, NULL);
        if (client < 0) continue;

        char garbage[512];
        read(client, garbage, sizeof(garbage));

        int fd = open(METRICS_FILE, O_RDONLY);
        int len = 0;
        if (fd >= 0) {
            len = read(fd, buffer, sizeof(buffer) - 1);
            close(fd);
        } else {
            len = sprintf(buffer, "# Metrics gathering...\n");
        }
        if (len < 0) len = 0;
        buffer[len] = 0;

        char header[256];
        int hlen = snprintf(header, sizeof(header), 
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", 
            len);

        write(client, header, hlen);
        write(client, buffer, len);
        
        shutdown(client, SHUT_RDWR);
        close(client);
    }
    return 0;
}
