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
#define PID_CACHE_FILE "/dev/shm/mag_pids.cache"
#define EXPORTER_VERSION "1.31"

static char buffer[32768];
static char big_read_buf[4096];

#define MAX_DYN_PROCS 50

typedef struct {
    char name[32];
    int pid;
} TargetProcess;

TargetProcess dyn_procs[MAX_DYN_PROCS];
int dyn_procs_count = 0;
int player_pid = -1;

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

void load_or_refresh_dynamic_pids() {
    struct stat st;
    int need_scan = 1;

    dyn_procs_count = 0;
    player_pid = -1;

    if (stat(PID_CACHE_FILE, &st) == 0 && (time(NULL) - st.st_mtime < 60)) {
        int fd = open(PID_CACHE_FILE, O_RDONLY);
        if (fd >= 0) {
            char cache_buf[2048];
            int n = read(fd, cache_buf, sizeof(cache_buf)-1);
            close(fd);
            if (n > 0) {
                cache_buf[n] = '\0';
                char* line = cache_buf;
                int all_alive = 1;

                while (line && *line) {
                    char* next = strchr(line, '\n');
                    if (next) *next = '\0';

                    char t_name[64]; int t_pid;
                    if (sscanf(line, "%63s %d", t_name, &t_pid) == 2) {
                        char stat_path[64];
                        snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", t_pid);
                        int sfd = open(stat_path, O_RDONLY | O_NONBLOCK);
                        if (sfd < 0) {
                            all_alive = 0;
                            break;
                        }
                        close(sfd);

                        if (strcmp(t_name, "PlayerApp") == 0) player_pid = t_pid;

                        if (dyn_procs_count < MAX_DYN_PROCS) {
                            strncpy(dyn_procs[dyn_procs_count].name, t_name, 31);
                            dyn_procs[dyn_procs_count].pid = t_pid;
                            dyn_procs_count++;
                        }
                    }
                    line = next ? next + 1 : NULL;
                }
                if (all_alive && player_pid != -1) need_scan = 0;
            }
        }
    }

    if (!need_scan) return;

    dyn_procs_count = 0;
    player_pid = -1;

    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            if (isdigit(ent->d_name[0])) {
                int pid = atoi(ent->d_name);
                char path[64], stat_buf[512];

                snprintf(path, sizeof(path), "/proc/%d/stat", pid);
                if (read_file_to_buf(path, stat_buf, sizeof(stat_buf))) {
                    char* op = strchr(stat_buf, '(');
                    char* cp = strrchr(stat_buf, ')');
                    if (op && cp) {
                        *cp = 0;
                        char* pname = op + 1;

                        if (player_pid == -1 && (strstr(pname, "ControlT") || strstr(pname, "stbapp"))) {
                            player_pid = pid;
                            if (dyn_procs_count < MAX_DYN_PROCS) {
                                strcpy(dyn_procs[dyn_procs_count].name, "PlayerApp");
                                dyn_procs[dyn_procs_count].pid = pid;
                                dyn_procs_count++;
                            }
                        }
                        else if (strncmp(pname, "AUD", 3) == 0 || strncmp(pname, "STVID", 5) == 0) {
                            if (dyn_procs_count < MAX_DYN_PROCS) {
                                char safe_name[32];
                                int j = 0;
                                for (int i = 0; pname[i] != '\0' && i < 31; i++) {
                                    if (isalnum(pname[i]) || pname[i] == '_' || pname[i] == '[' || pname[i] == ']' || pname[i] == '.') {
                                        safe_name[j++] = pname[i];
                                    }
                                }
                                safe_name[j] = '\0';
                                strncpy(dyn_procs[dyn_procs_count].name, safe_name, 31);
                                dyn_procs[dyn_procs_count].pid = pid;
                                dyn_procs_count++;
                            }
                        }
                    }
                }
            }
        }
        closedir(dir);
    }

    int fd = open(PID_CACHE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char out[2048] = {0};
        int off = 0;
        for (int i = 0; i < dyn_procs_count; i++) {
            off += snprintf(out+off, sizeof(out)-off, "%s %d\n", dyn_procs[i].name, dyn_procs[i].pid);
        }
        write(fd, out, off);
        close(fd);
    }
}

void collect_once() {
    int fd = open(TEMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) exit(1);

    load_or_refresh_dynamic_pids();

    int len = 0;
    int max_len = sizeof(buffer);

    int total_procs = 0;
    DIR* p_dir = opendir("/proc");
    if (p_dir) {
        struct dirent* e;
        while ((e = readdir(p_dir)) != NULL) {
            if (isdigit(e->d_name[0])) total_procs++;
        }
        closedir(p_dir);
    }

    int udp_cnt = 0;
    char current_udp_ip[32] = "none";
    if (read_file_to_buf("/proc/net/udp", big_read_buf, sizeof(big_read_buf))) {
        char* p = big_read_buf;
        while ((p = strchr(p, '\n')) != NULL) {
            p++; unsigned int addr;
            if (sscanf(p, "%*d: %x:", &addr) == 1) {
                unsigned char b1 = (addr) & 0xFF;
                unsigned char b2 = (addr >> 8) & 0xFF;
                unsigned char b3 = (addr >> 16) & 0xFF;
                unsigned char b4 = (addr >> 24) & 0xFF;
                if (b1 >= 0xE0 && b1 <= 0xEF) {
                    udp_cnt++;
                    snprintf(current_udp_ip, sizeof(current_udp_ip), "%d.%d.%d.%d", b1, b2, b3, b4);
                }
            }
        }
    }

    char video_codec[16] = "none";
    for (int i = 0; i < dyn_procs_count; i++) {
        if (strstr(dyn_procs[i].name, "H264")) strcpy(video_codec, "h264");
        else if (strstr(dyn_procs[i].name, "MPEG2")) strcpy(video_codec, "mpeg2");
    }

    len = append_metric(len, max_len, "mag250_device_info{version=\"%s\",fw_desc=\"%s\",mac=\"%s\",codec=\"%s\",udp_addr=\"%s\"} 1\n", 
        EXPORTER_VERSION, global_fw_desc, global_mac_addr, video_codec, current_udp_ip);
    
    len = append_metric(len, max_len, "node_procs_total %d\n", total_procs);

    if (read_file_to_buf("/sys/class/thermal/thermal_zone0/temp", big_read_buf, sizeof(big_read_buf))) {
        long temp_milli = atol(big_read_buf);
        if (temp_milli > 0) {
            double temp_celsius = (double)temp_milli / 1000.0;
            len = append_metric(len, max_len, "node_temperature_celsius %.2f\n", temp_celsius);
        }
    }

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

    len = append_metric(len, max_len, "mag250_udp_streams_total %d\n", udp_cnt);

    for (int i = 0; i < dyn_procs_count; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/stat", dyn_procs[i].pid);

        if (read_file_to_buf(path, big_read_buf, sizeof(big_read_buf))) {
            char* cp = strrchr(big_read_buf, ')');
            if (cp) {
                char state; unsigned long ut, st;
                if (sscanf(cp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &state, &ut, &st) >= 3) {
                    len = append_metric(len, max_len, 
                        "mag250_process_cpu_ticks{name=\"%s\",mode=\"user\"} %lu\n"
                        "mag250_process_cpu_ticks{name=\"%s\",mode=\"system\"} %lu\n"
                        "mag250_process_blocked{name=\"%s\"} %d\n", 
                        dyn_procs[i].name, ut, dyn_procs[i].name, st, dyn_procs[i].name, (state == 'D'));
                }
            }
        } else {
            len = append_metric(len, max_len, "mag250_process_blocked{name=\"%s\"} 0\n", dyn_procs[i].name);
        }
    }

    int hw_vid = 0, hw_aud = 0;
    if (player_pid != -1) {
        char fd_dir_path[64];
        snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", player_pid);
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

    signal(SIGCHLD, SIG_DFL);
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

    printf("Exporter v%s started. Port: %d\n", EXPORTER_VERSION, PORT);
    fflush(stdout);

    time_t last_spawn = 0;
    pid_t active_child = 0;

    while (1) {
        int wstat;
        pid_t dead_pid;
        while ((dead_pid = waitpid(-1, &wstat, WNOHANG)) > 0) {
            if (dead_pid == active_child) active_child = 0;
        }

        time_t now = time(NULL);

        if (expire_time > 0 && now >= expire_time) {
            unlink(METRICS_FILE); unlink(PID_CACHE_FILE);
            exit(0);
        }

        if (active_child == 0 && (now - last_spawn >= 5 || now < last_spawn)) {
            active_child = fork();
            if (active_child == 0) {
                collect_once();
            } else if (active_child > 0) {
                last_spawn = now;
            }
        } 
        else if (active_child > 0 && (now - last_spawn > 5)) {
            kill(active_child, SIGKILL);
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv = {1, 0};

        int sel = select(server_fd + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0) continue;

        int client = accept(server_fd, NULL, NULL);
        if (client < 0) { usleep(50000); continue; }

        struct timeval net_tv = {2, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&net_tv, sizeof(net_tv));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char*)&net_tv, sizeof(net_tv));

        char req[512] = {0};
        if (read(client, req, sizeof(req)-1) <= 0) {
            close(client);
            continue;
        }

        int is_quit = (strncmp(req, "GET /quit", 9) == 0);
        int is_debug = (strncmp(req, "GET /debug", 10) == 0);
        int is_restart = (strncmp(req, "GET /restart", 12) == 0);

        int len = 0;
        if (is_quit) {
            len = sprintf(buffer, "Shutting down.\n");
        } else if (is_restart) {
            if (active_child > 0) kill(active_child, SIGKILL);
            active_child = 0;
            unlink(PID_CACHE_FILE);
            len = sprintf(buffer, "Force restarted collector cache.\n");
        } else if (is_debug) {
            len = sprintf(buffer, "Version: %s\nActive Child PID: %d\nLast Spawn Time: %ld\n", EXPORTER_VERSION, active_child, last_spawn);
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
 
        close(client);

        if (is_quit) {
            if (active_child > 0) kill(active_child, SIGKILL);
            unlink(METRICS_FILE); unlink(PID_CACHE_FILE);
            exit(0);
        }
    }
    return 0;
}
