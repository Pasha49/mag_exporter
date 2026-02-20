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
#include <time.h>
#include <ctype.h>
#include <stdarg.h>

#define PORT 9100
#define METRICS_FILE "/dev/shm/mag_metrics.prom"
#define TEMP_FILE "/dev/shm/mag_metrics.tmp"
#define EXPORTER_VERSION "1.18"

static char buffer[32768]; 
static char big_read_buf[4096]; 

typedef struct {
    const char* name;
    int pid;
} TargetProcess;

TargetProcess targets[] = {
    {"MAG250_ControlT", -1},
    {"AUD[0].PESTask", -1},
    {"AUD[0].DecTask", -1},
    {"AUD[0].MixTask", -1},
    {"STVID[0].H264Pa", -1},
    {"STVID[0].Produc", -1},
    {"STVID[0].Displa", -1},
    {NULL, -1}
};

char global_fw_desc[128] = "unknown";
char global_mac_addr[32] = "unknown";

int read_file_to_buf(const char* path, char* dest, size_t max) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { dest[0] = 0; return 0; }
    int n = read(fd, dest, max - 1);
    close(fd);
    if (n > 0) { dest[n] = 0; return 1; }
    dest[0] = 0;
    return 0;
}

int append_metric(int len, int max_len, const char* format, ...) {
    if (max_len - len < 512) return len;
    va_list args;
    va_start(args, format);
    int added = vsnprintf(buffer + len, max_len - len, format, args);
    va_end(args);
    if (added > 0 && added < max_len - len) return len + added;
    return len;
}

void refresh_pids() {
    DIR* dir = opendir("/proc");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (isdigit(ent->d_name[0])) {
            int pid = atoi(ent->d_name);
            char path[64], cmd[256];
            
            snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
            if (read_file_to_buf(path, cmd, sizeof(cmd))) {
                for (int i = 0; targets[i].name; i++) {
                    if (targets[i].pid == -1 && strstr(cmd, targets[i].name)) targets[i].pid = pid;
                }
            }
            snprintf(path, sizeof(path), "/proc/%d/stat", pid);
            if (read_file_to_buf(path, cmd, sizeof(cmd))) {
                char* op = strchr(cmd, '(');
                char* cp = strrchr(cmd, ')');
                if (op && cp) {
                    *cp = 0;
                    for (int i = 0; targets[i].name; i++) {
                        if (targets[i].pid == -1 && strcmp(op + 1, targets[i].name) == 0) targets[i].pid = pid;
                    }
                }
            }
        }
    }
    closedir(dir);
}

void collect_once() {
    int fd = open(TEMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) exit(1);

    int len = 0;
    int max_len = sizeof(buffer);

    len = append_metric(len, max_len, "mag250_device_info{version=\"%s\",fw_desc=\"%s\",mac=\"%s\"} 1\n", EXPORTER_VERSION, global_fw_desc, global_mac_addr);

    if (read_file_to_buf("/proc/uptime", big_read_buf, sizeof(big_read_buf))) {
        double up; if(sscanf(big_read_buf, "%lf", &up)) len = append_metric(len, max_len, "node_uptime_seconds %.2f\n", up);
    }
    len = append_metric(len, max_len, "mag_exporter_last_update_timestamp %llu\n", (unsigned long long)time(NULL));

    if (read_file_to_buf("/proc/loadavg", big_read_buf, sizeof(big_read_buf))) {
        double l1, l5, l15;
        if (sscanf(big_read_buf, "%lf %lf %lf", &l1, &l5, &l15) == 3)
            len = append_metric(len, max_len, "node_load1 %.2f\nnode_load5 %.2f\nnode_load15 %.2f\n", l1, l5, l15);
    }

    if (read_file_to_buf("/proc/stat", big_read_buf, sizeof(big_read_buf))) {
        unsigned long long u, n, s, i, iw;
        char* p = strstr(big_read_buf, "cpu ");
        if (p && sscanf(p, "cpu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &iw) >= 4) {
            len = append_metric(len, max_len, 
                "node_cpu_seconds_total{mode=\"user\"} %.2f\nnode_cpu_seconds_total{mode=\"system\"} %.2f\n"
                "node_cpu_seconds_total{mode=\"idle\"} %.2f\nnode_cpu_seconds_total{mode=\"iowait\"} %.2f\n",
                (double)u/100.0, (double)s/100.0, (double)i/100.0, (double)iw/100.0);
        }
    }

    if (read_file_to_buf("/proc/meminfo", big_read_buf, sizeof(big_read_buf))) {
        unsigned long mt=0, mf=0, mb=0, mc=0;
        char* p = strstr(big_read_buf, "MemTotal:"); if(p) sscanf(p, "MemTotal: %lu", &mt);
        p = strstr(big_read_buf, "MemFree:"); if(p) sscanf(p, "MemFree: %lu", &mf);
        p = strstr(big_read_buf, "Buffers:"); if(p) sscanf(p, "Buffers: %lu", &mb);
        p = strstr(big_read_buf, "Cached:"); if(p) sscanf(p, "Cached: %lu", &mc);
        len = append_metric(len, max_len, 
            "node_memory_MemTotal_bytes %lu\nnode_memory_MemFree_bytes %lu\n"
            "node_memory_Buffers_bytes %lu\nnode_memory_Cached_bytes %lu\n", mt*1024, mf*1024, mb*1024, mc*1024);
    }

    if (read_file_to_buf("/proc/net/dev", big_read_buf, sizeof(big_read_buf))) {
        char* p = strstr(big_read_buf, "eth0:");
        if (p) {
            unsigned long long rx, rx_err, tx;
            if (sscanf(p + 5, "%llu %*u %llu %*u %*u %*u %*u %*u %llu", &rx, &rx_err, &tx) >= 3) {
                len = append_metric(len, max_len, 
                    "node_network_receive_bytes_total %llu\nnode_network_receive_errors_total %llu\nnode_network_transmit_bytes_total %llu\n", rx, rx_err, tx);
            }
        }
    }

    if (read_file_to_buf("/proc/net/udp", big_read_buf, sizeof(big_read_buf))) {
        int udp_cnt = 0;
        char* p = big_read_buf;
        while ((p = strchr(p, '\n')) != NULL) {
            p++; unsigned int addr;
            if (sscanf(p, "%*d: %x:", &addr) == 1 && ((unsigned char)(addr & 0xFF) >= 0xE0)) udp_cnt++;
        }
        len = append_metric(len, max_len, "mag250_udp_streams_total %d\n", udp_cnt);
    }

    int stbapp_pid = -1;
    for (int i = 0; targets[i].name; i++) {
        if (strcmp(targets[i].name, "MAG250_ControlT") == 0) stbapp_pid = targets[i].pid;
        
        if (targets[i].pid != -1) {
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/stat", targets[i].pid);
            if (read_file_to_buf(path, big_read_buf, sizeof(big_read_buf))) {
                char* cp = strrchr(big_read_buf, ')');
                if (cp) {
                    char state; unsigned long ut, st;
                    if (sscanf(cp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st) >= 3) {
                        len = append_metric(len, max_len, 
                            "mag250_process_cpu_ticks{name=\"%s\",mode=\"user\"} %lu\n"
                            "mag250_process_cpu_ticks{name=\"%s\",mode=\"system\"} %lu\n"
                            "mag250_process_blocked{name=\"%s\"} %d\n", 
                            targets[i].name, ut, targets[i].name, st, targets[i].name, (state == 'D'));
                    }
                }
            }
        }
        if (targets[i].pid == -1) len = append_metric(len, max_len, "mag250_process_blocked{name=\"%s\"} 0\n", targets[i].name);
    }

    int hw_vid = 0, hw_aud = 0;
    if (stbapp_pid != -1) {
        char fd_dir_path[64];
        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", stbapp_pid);
        DIR* fd_dir = opendir(fd_dir_path);
        if (fd_dir) {
            struct dirent* fd_ent;
            while ((fd_ent = readdir(fd_dir)) != NULL) {
                if (isdigit(fd_ent->d_name[0])) {
                    char fd_path[128], link_target[256];
                    snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_dir_path, fd_ent->d_name);
                    int r = readlink(fd_path, link_target, sizeof(link_target)-1);
                    if (r > 0) {
                        link_target[r] = '\0';
                        if (strstr(link_target, "stvid_ioctl")) hw_vid = 1;
                        if (strstr(link_target, "staudlx_ioctl")) hw_aud = 1;
                    }
                }
            }
            closedir(fd_dir);
        }
    }
    len = append_metric(len, max_len, "mag250_hw_decoder_active{type=\"video\"} %d\n", hw_vid);
    len = append_metric(len, max_len, "mag250_hw_decoder_active{type=\"audio\"} %d\n", hw_aud);

    write(fd, buffer, len);
    close(fd);
    rename(TEMP_FILE, METRICS_FILE);

    exit(0);
}

int main(int argc, char *argv[]) {
    time_t expire_time = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            int timeout = atoi(argv[i+1]);
            if (timeout > 0) expire_time = time(NULL) + timeout;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    FILE *fp = popen("fw_printenv 2>/dev/null", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "Image_Desc=", 11) == 0) sscanf(line+11, "%127[^\n]", global_fw_desc);
            else if (strncmp(line, "ethaddr=", 8) == 0) sscanf(line+8, "%31[^\n]", global_mac_addr);
        }
        pclose(fp);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    listen(server_fd, 10);

    printf("Stateless Exporter v%s started. Port: %d\n", EXPORTER_VERSION, PORT);
    fflush(stdout);

    time_t last_spawn = 0;
    time_t last_pid_refresh = 0;
    pid_t active_child = 0;

    while (1) {
        int wstat;
        pid_t dead_pid;
        while ((dead_pid = waitpid(-1, &wstat, WNOHANG)) > 0) {
            if (dead_pid == active_child) active_child = 0;
        }

        time_t now = time(NULL);

        if (expire_time > 0 && now >= expire_time) {
            unlink(METRICS_FILE);
            exit(0);
        }

        if (now - last_pid_refresh > 60 || now < last_pid_refresh) {
            refresh_pids();
            last_pid_refresh = now;
        }

        if (active_child == 0 && (now - last_spawn >= 5 || now < last_spawn)) {
            active_child = fork();
            if (active_child == 0) {
                collect_once();
            }
            last_spawn = now;
        }
        else if (active_child > 0 && (now - last_spawn > 5)) {
            kill(active_child, SIGKILL);
            waitpid(active_child, &wstat, 0); // Добиваем
            active_child = 0;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv = {1, 0};

        int sel = select(server_fd + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0) continue; 

        int client = accept(server_fd, NULL, NULL);
        if (client < 0) { usleep(50000); continue; }

        int flags = fcntl(client, F_GETFL, 0);
        fcntl(client, F_SETFL, flags | O_NONBLOCK);

        char req[512] = {0};
        read(client, req, sizeof(req)-1);

        int is_quit = (strncmp(req, "GET /quit", 9) == 0);
        int is_debug = (strncmp(req, "GET /debug", 10) == 0);

        int len = 0;
        if (is_quit) {
            len = sprintf(buffer, "Shutting down.\n");
        } else if (is_debug) {
            len = sprintf(buffer, "Stateless Architecture Active.\nChild PID: %d\nLast Spawn: %ld\n", active_child, last_spawn);
        } else {
            int fd = open(METRICS_FILE, O_RDONLY);
            if (fd >= 0) { len = read(fd, buffer, sizeof(buffer) - 1); close(fd); }
            else { len = sprintf(buffer, "# Gathering...\n"); }
            if (len < 0) len = 0;
        }

        buffer[len] = 0;
        char header[256];
        int hlen = snprintf(header, sizeof(header), 
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", len);

        write(client, header, hlen);
        write(client, buffer, len);
        
        shutdown(client, SHUT_RDWR);
        close(client);

        if (is_quit) {
            if (active_child > 0) kill(active_child, SIGKILL);
            unlink(METRICS_FILE);
            exit(0);
        }
    }
    return 0;
}
