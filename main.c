/*
 * PS5 BD-Mute Daemon
 * Author: Akut0
 * Description: Automatically controls the PS5 Blu-ray drive power state
 *              based on running applications to reduce noise and power.
 *
 * /data/bd_mute_enabled
 *   → If present at start: BD is powered OFF and stays OFF
 *     (even after games) until BD_ON or DISABLE clears the force flag.
 *
 * /data/bd_mute_exclude.txt
 *   → One Title-ID per line (e.g. CUSA12345 or PPSA67890).
 *     Games in this list are ignored by the auto-mute logic.
 *
 * TCP Port 9033 commands:
 *   ENABLE   - activate auto-mute logic
 *   DISABLE  - deactivate auto-mute logic (BD is forced ON)
 *   TOGGLE   - switch auto-mute on/off
 *   STATUS   - current state
 *   BD_ON    - force Blu-ray drive power ON
 *   BD_OFF   - force Blu-ray drive power OFF
 *   RELOAD   - reload exclude list from file
 *   QUIT     - shut down the daemon
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BD_MUTE_VERSION        "1.1.1"
#define LOG_FILE_PATH          "/data/bd_mute.log"
#define ENABLE_FILE_PATH       "/data/bd_mute_enabled"
#define EXCLUDE_FILE_PATH      "/data/bd_mute_exclude.txt"
#define MAX_LOG_SIZE           (100 * 1024)

#define SYSCTL_ENTRY_MIN_SIZE  76
#define SYSCTL_PID_OFFSET      72
#define MAX_SYSCTL_ENTRIES     512

#define IDLE_POLL_INTERVAL_SEC 2
#define GAME_POLL_INTERVAL_SEC 1

#define TCP_PORT               9033
#define TCP_BACKLOG            4
#define TCP_BUF_SIZE           128

#define MAX_EXCLUDE            64
#define TITLE_ID_LEN           16

typedef int (*bd_fn)(int);
typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} app_info_t;

typedef int (*appinfo_fn)(pid_t, app_info_t *);

typedef struct {
    char pad[45];
    char message[3075];
} notify_request_t;

typedef int (*notify_fn)(int, notify_request_t *, size_t, int);

/* ------------------------------------------------------------------ */
/* Globals                                                            */
/* ------------------------------------------------------------------ */
static bd_fn          g_bd       = NULL;
static appinfo_fn     g_appinfo  = NULL;
static notify_fn      g_notify   = NULL;
static volatile sig_atomic_t g_running = 1;

static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int            g_auto_enabled = 1;
static int            g_last_state   = -1;  /* -1 unknown, 0 = BD on, 1 = BD off */
static int            g_off_streak   = 0;
static int            g_force_off    = 0;   /* boot-file: keep BD off until cleared */

/* Exclusion list */
static char g_exclude[MAX_EXCLUDE][TITLE_ID_LEN];
static int  g_exclude_count = 0;

static const char *g_exclude_defaults[] = {
    /* "CUSA12345", */
    /* "PPSA67890", */
    NULL
};

/* ------------------------------------------------------------------ */
/* Signal handler                                                     */
/* ------------------------------------------------------------------ */
static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Logging – file only vs. file + notification                        */
/* ------------------------------------------------------------------ */
static void logline(const char *s) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_buf);

    FILE *f_check = fopen(LOG_FILE_PATH, "r");
    if (f_check) {
        fseek(f_check, 0, SEEK_END);
        long size = ftell(f_check);
        fclose(f_check);
        if (size > MAX_LOG_SIZE) {
            FILE *f_reset = fopen(LOG_FILE_PATH, "w");
            if (f_reset) fclose(f_reset);
        }
    }

    FILE *f = fopen(LOG_FILE_PATH, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", timestamp, s);
        fclose(f);
    }
}

static void logline_notify(const char *s) {
    logline(s);
    if (g_notify) {
        notify_request_t req;
        memset(&req, 0, sizeof(req));
        snprintf(req.message, sizeof(req.message), "%s", s);
        g_notify(0, &req, sizeof(req), 0);
    }
}

/* ------------------------------------------------------------------ */
/* BD power control                                                   */
/* ------------------------------------------------------------------ */
static int set_bd(int on) {
    if (!g_bd) return -1;
    int ret = g_bd(on ? 1 : 0);
    if (ret != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "set_bd(%s) failed, code=%d", on ? "ON" : "OFF", ret);
        logline(msg);
    }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Enable-file helper                                                 */
/* ------------------------------------------------------------------ */
static int file_exists(void) {
    return access(ENABLE_FILE_PATH, F_OK) == 0;
}

/* ------------------------------------------------------------------ */
/* Exclusion list                                                     */
/* ------------------------------------------------------------------ */
static int is_excluded(const char *title_id) {
    for (int i = 0; i < g_exclude_count; i++) {
        if (strcasecmp(g_exclude[i], title_id) == 0)
            return 1;
    }
    return 0;
}

static void add_exclude(const char *id) {
    if (!id || !id[0] || g_exclude_count >= MAX_EXCLUDE)
        return;
    if (is_excluded(id))
        return;
    strncpy(g_exclude[g_exclude_count], id, TITLE_ID_LEN - 1);
    g_exclude[g_exclude_count][TITLE_ID_LEN - 1] = '\0';
    g_exclude_count++;
}

static void load_exclude_list(void) {
    g_exclude_count = 0;

    for (int i = 0; g_exclude_defaults[i] != NULL; i++)
        add_exclude(g_exclude_defaults[i]);

    FILE *f = fopen(EXCLUDE_FILE_PATH, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f) && g_exclude_count < MAX_EXCLUDE) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            size_t len = strlen(p);
            while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' ||
                               p[len - 1] == ' '  || p[len - 1] == '\t')) {
                p[--len] = '\0';
            }
            if (len == 0 || p[0] == '#' || p[0] == ';')
                continue;
            add_exclude(p);
        }
        fclose(f);
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "Exclude list loaded: %d title(s)", g_exclude_count);
    logline(msg);
}

/* ------------------------------------------------------------------ */
/* Game detection                                                     */
/* ------------------------------------------------------------------ */
static int game_running(char *seen, size_t seen_len) {
    int mib[4] = {1, 14, 8, 0};
    size_t buf_size = 0;
    uint8_t *buf;
    size_t offset;
    size_t entries = 0;
    int found = 0;

    seen[0] = '\0';
    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) || buf_size == 0)
        return 0;

    buf = malloc(buf_size);
    if (!buf) return 0;

    if (sysctl(mib, 4, buf, &buf_size, NULL, 0)) {
        free(buf);
        return 0;
    }

    for (offset = 0;
         offset + SYSCTL_ENTRY_MIN_SIZE <= buf_size && entries < MAX_SYSCTL_ENTRIES;
         entries++)
    {
        int entry_size;
        pid_t pid;
        app_info_t info;

        memcpy(&entry_size, buf + offset, sizeof(entry_size));
        if (entry_size < SYSCTL_ENTRY_MIN_SIZE ||
            (size_t)entry_size > buf_size - offset)
        {
            break;
        }

        memcpy(&pid, buf + offset + SYSCTL_PID_OFFSET, sizeof(pid));
        memset(&info, 0, sizeof(info));

        if (pid > 0 && g_appinfo(pid, &info) == 0 && info.title_id[0] != '\0') {
            info.title_id[sizeof(info.title_id) - 1] = '\0';

            if (!strstr(seen, info.title_id)) {
                if (seen[0] != '\0')
                    strncat(seen, " ", seen_len - strlen(seen) - 1);
                strncat(seen, info.title_id, seen_len - strlen(seen) - 1);
            }

            if ((!strncmp(info.title_id, "PPSA", 4) ||
                 !strncmp(info.title_id, "CUSA", 4)) &&
                !is_excluded(info.title_id))
            {
                found = 1;
            }
        }
        offset += (size_t)entry_size;
    }

    free(buf);
    return found;
}

/* ------------------------------------------------------------------ */
/* Status string                                                      */
/* ------------------------------------------------------------------ */
static void build_status(char *buf, size_t len) {
    pthread_mutex_lock(&g_state_mutex);
    int auto_en   = g_auto_enabled;
    int last      = g_last_state;
    int force_off = g_force_off;
    pthread_mutex_unlock(&g_state_mutex);

    const char *bd_str = (last < 0) ? "unknown" :
                         (last == 1) ? "OFF" : "ON";

    snprintf(buf, len,
             "BD-Mute v%s | auto=%s | BD=%s | force_off=%s | boot_file=%s | exclude=%d",
             BD_MUTE_VERSION,
             auto_en ? "ON" : "OFF",
             bd_str,
             force_off ? "yes" : "no",
             file_exists() ? "present" : "missing",
             g_exclude_count);
}

/* ------------------------------------------------------------------ */
/* TCP command handler                                                */
/* ------------------------------------------------------------------ */
static void handle_tcp_cmd(int client_fd, const char *cmd) {
    char reply[256];
    reply[0] = '\0';

    if (strcasecmp(cmd, "ENABLE") == 0) {
        pthread_mutex_lock(&g_state_mutex);
        g_auto_enabled = 1;
        pthread_mutex_unlock(&g_state_mutex);
        logline_notify("TCP: ENABLE -> auto-mute ON");
        snprintf(reply, sizeof(reply), "OK auto=ON\n");
    }
    else if (strcasecmp(cmd, "DISABLE") == 0) {
        pthread_mutex_lock(&g_state_mutex);
        g_auto_enabled = 0;
        g_force_off    = 0;
        g_last_state   = 0;
        g_off_streak   = 0;
        pthread_mutex_unlock(&g_state_mutex);
        set_bd(1);
        logline_notify("TCP: DISABLE -> auto-mute OFF (BD forced ON)");
        snprintf(reply, sizeof(reply), "OK auto=OFF\n");
    }
    else if (strcasecmp(cmd, "TOGGLE") == 0) {
        int now;
        pthread_mutex_lock(&g_state_mutex);
        g_auto_enabled = !g_auto_enabled;
        now = g_auto_enabled;
        if (!now) {
            g_force_off  = 0;
            g_last_state = 0;
            g_off_streak = 0;
        }
        pthread_mutex_unlock(&g_state_mutex);
        if (!now)
            set_bd(1);
        char msg[64];
        snprintf(msg, sizeof(msg), "TCP: TOGGLE -> auto-mute %s", now ? "ON" : "OFF");
        logline_notify(msg);
        snprintf(reply, sizeof(reply), "OK auto=%s\n", now ? "ON" : "OFF");
    }
    else if (strcasecmp(cmd, "STATUS") == 0) {
        build_status(reply, sizeof(reply));
        strcat(reply, "\n");
    }
    else if (strcasecmp(cmd, "BD_ON") == 0) {
        pthread_mutex_lock(&g_state_mutex);
        g_force_off  = 0;
        g_last_state = 0;
        g_off_streak = 0;
        pthread_mutex_unlock(&g_state_mutex);
        set_bd(1);
        logline_notify("TCP: BD_ON");
        snprintf(reply, sizeof(reply), "OK BD=ON\n");
    }
    else if (strcasecmp(cmd, "BD_OFF") == 0) {
        pthread_mutex_lock(&g_state_mutex);
        g_last_state = 1;
        pthread_mutex_unlock(&g_state_mutex);
        set_bd(0);
        logline_notify("TCP: BD_OFF");
        snprintf(reply, sizeof(reply), "OK BD=OFF\n");
    }
    else if (strcasecmp(cmd, "RELOAD") == 0) {
        load_exclude_list();
        snprintf(reply, sizeof(reply), "OK exclude=%d\n", g_exclude_count);
    }
    else if (strcasecmp(cmd, "QUIT") == 0 || strcasecmp(cmd, "EXIT") == 0) {
        logline("TCP: QUIT -> shutting down");
        g_running = 0;
        snprintf(reply, sizeof(reply), "OK quitting\n");
    }
    else {
        snprintf(reply, sizeof(reply),
                 "ERR unknown. Commands: ENABLE|DISABLE|TOGGLE|STATUS|BD_ON|BD_OFF|RELOAD|QUIT\n");
    }

    if (reply[0])
        send(client_fd, reply, strlen(reply), 0);
}

/* ------------------------------------------------------------------ */
/* TCP server thread                                                  */
/* ------------------------------------------------------------------ */
static void *tcp_server_thread(void *arg) {
    (void)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        logline("TCP: socket() failed");
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(TCP_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        logline("TCP: bind() failed");
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, TCP_BACKLOG) < 0) {
        logline("TCP: listen() failed");
        close(server_fd);
        return NULL;
    }

    fcntl(server_fd, F_SETFL, O_NONBLOCK);

    char msg[64];
    snprintf(msg, sizeof(msg), "TCP server listening on port %d", TCP_PORT);
    logline(msg);

    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(200000);
                continue;
            }
            break;
        }

        char buf[TCP_BUF_SIZE];
        memset(buf, 0, sizeof(buf));
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\r' || buf[i] == '\n') {
                    buf[i] = '\0';
                    break;
                }
            }
            if (buf[0])
                handle_tcp_cmd(client_fd, buf);
        }
        close(client_fd);
    }

    close(server_fd);
    logline("TCP server stopped");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */
int main(void) {
    void *k_handle = NULL;
    int tcp_ok = 0;
    pthread_t tcp_thread;

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* Resolve symbols */
    g_bd      = (bd_fn)dlsym(RTLD_DEFAULT, "sceKernelIccControlBDPowerState");
    g_appinfo = (appinfo_fn)dlsym(RTLD_DEFAULT, "sceKernelGetAppInfo");
    g_notify  = (notify_fn)dlsym(RTLD_DEFAULT, "sceKernelSendNotificationRequest");

    if (!g_bd || !g_appinfo || !g_notify) {
        k_handle = dlopen("libkernel.sprx", RTLD_LAZY);
        if (!k_handle)
            k_handle = dlopen("libkernel_web.sprx", RTLD_LAZY);
        if (k_handle) {
            if (!g_bd)
                g_bd = (bd_fn)dlsym(k_handle, "sceKernelIccControlBDPowerState");
            if (!g_appinfo)
                g_appinfo = (appinfo_fn)dlsym(k_handle, "sceKernelGetAppInfo");
            if (!g_notify)
                g_notify = (notify_fn)dlsym(k_handle, "sceKernelSendNotificationRequest");
        }
    }

    if (!g_bd || !g_appinfo) {
        logline(!g_bd ? "Error: sceKernelIccControlBDPowerState missing"
                      : "Error: sceKernelGetAppInfo missing");
        if (k_handle) dlclose(k_handle);
        return 1;
    }

    load_exclude_list();

    /* Boot: force BD off and keep it off until BD_ON / DISABLE */
    if (file_exists()) {
        set_bd(0);
        g_last_state = 1;
        g_force_off  = 1;
        logline_notify("Boot file present -> BD powered OFF (force_off=1)");
    } else {
        logline("No boot file -> BD left untouched");
    }

    char start_msg[160];
    snprintf(start_msg, sizeof(start_msg),
             "BD-Mute Daemon v%s initialized\nMade with ♥ by Akut0",
             BD_MUTE_VERSION);
    logline_notify(start_msg);

    if (pthread_create(&tcp_thread, NULL, tcp_server_thread, NULL) == 0)
        tcp_ok = 1;
    else
        logline("Warning: could not start TCP thread");

    int current_poll_interval = IDLE_POLL_INTERVAL_SEC;

    while (g_running) {
        pthread_mutex_lock(&g_state_mutex);
        int auto_on = g_auto_enabled;
        pthread_mutex_unlock(&g_state_mutex);

        if (!auto_on) {
            sleep(IDLE_POLL_INTERVAL_SEC);
            continue;
        }

        char seen[256];
        int is_game = game_running(seen, sizeof(seen));

        pthread_mutex_lock(&g_state_mutex);

        if (is_game) {
            g_off_streak = 0;
            /* g_force_off stays set – BD remains off after game ends */
            current_poll_interval = GAME_POLL_INTERVAL_SEC;

            if (g_last_state != 1) {
                pthread_mutex_unlock(&g_state_mutex);
                logline_notify("BD-Mute: Game started -> BD off");
                set_bd(0);
                pthread_mutex_lock(&g_state_mutex);
                g_last_state = 1;
            }
        }
        else if (g_last_state == 1 && !g_force_off) {
            /* restore BD only when force_off is not active */
            current_poll_interval = GAME_POLL_INTERVAL_SEC;

            if (++g_off_streak >= 2) {
                pthread_mutex_unlock(&g_state_mutex);
                logline_notify("BD-Mute: Game closed -> BD on");
                set_bd(1);
                pthread_mutex_lock(&g_state_mutex);
                g_last_state = 0;
                g_off_streak = 0;
                current_poll_interval = IDLE_POLL_INTERVAL_SEC;
            }
        }
        else if (g_last_state < 0) {
            g_last_state = 0;
            current_poll_interval = IDLE_POLL_INTERVAL_SEC;
        }

        pthread_mutex_unlock(&g_state_mutex);

        sleep(current_poll_interval);
    }

    logline_notify("BD-Mute: Shutting down daemon...");
    set_bd(1);

    if (tcp_ok)
        pthread_join(tcp_thread, NULL);

    if (k_handle)
        dlclose(k_handle);

    return 0;
}
