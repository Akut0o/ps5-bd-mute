/*
 * PS5 BD-Mute Daemon
 * Author: Akut0
 * Description: Automatically controls the PS5 Blu-ray drive power state
 *              based on running applications to reduce noise and power.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define BD_MUTE_VERSION "1.0.2"
#define LOG_FILE_PATH   "/data/bd_mute.log"
#define MAX_LOG_SIZE    (100 * 1024) // Max 100 KB Log-Größe (automatische Rotation)

#define SYSCTL_ENTRY_MIN_SIZE 76
#define SYSCTL_PID_OFFSET     72
#define MAX_SYSCTL_ENTRIES    512

#define IDLE_POLL_INTERVAL_SEC 2
#define GAME_POLL_INTERVAL_SEC 1

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

// Globale Variablen
static bd_fn g_bd = NULL;
static appinfo_fn g_appinfo = NULL;
static notify_fn g_notify = NULL;
static volatile sig_atomic_t g_running = 1;

// Signal-Handler für sauberes Beenden (SIGINT, SIGTERM)
static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

// Log-Datei verwalten (mit Zeitstempel und automatischer Größenbegrenzung)
static void logline(const char *s) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_buf);

    // Prüfen, ob die Log-Datei zu groß wird (Rotation/Truncate)
    FILE *f_check = fopen(LOG_FILE_PATH, "r");
    if (f_check) {
        fseek(f_check, 0, SEEK_END);
        long size = ftell(f_check);
        fclose(f_check);
        if (size > MAX_LOG_SIZE) {
            FILE *f_reset = fopen(LOG_FILE_PATH, "w"); // Datei leeren
            if (f_reset) fclose(f_reset);
        }
    }

    FILE *f = fopen(LOG_FILE_PATH, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", timestamp, s);
        fclose(f);
    }

    if (g_notify) {
        notify_request_t req;
        memset(&req, 0, sizeof(req));
        snprintf(req.message, sizeof(req.message), "%s", s);
        g_notify(0, &req, sizeof(req), 0);
    }
}

static int set_bd(int on) {
    int ret = g_bd(on ? 1 : 0);
    return ret;
}

static int game_running(char *seen, size_t seen_len) {
    int mib[4] = {1, 14, 8, 0};
    size_t buf_size = 0;
    uint8_t *buf;
    size_t offset;
    size_t entries = 0;
    int found = 0;

    seen[0] = '\0';
    if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) || buf_size == 0) return 0;
    
    buf = malloc(buf_size);
    if (!buf) return 0;
    
    if (sysctl(mib, 4, buf, &buf_size, NULL, 0)) {
        free(buf);
        return 0;
    }

    for (offset = 0; offset + SYSCTL_ENTRY_MIN_SIZE <= buf_size && entries < MAX_SYSCTL_ENTRIES; entries++) {
        int entry_size;
        pid_t pid;
        app_info_t info;

        memcpy(&entry_size, buf + offset, sizeof(entry_size));
        if (entry_size < SYSCTL_ENTRY_MIN_SIZE || (size_t)entry_size > buf_size - offset) {
            break;
        }

        memcpy(&pid, buf + offset + SYSCTL_PID_OFFSET, sizeof(pid));
        memset(&info, 0, sizeof(info));

        if (pid > 0 && g_appinfo(pid, &info) == 0 && info.title_id[0] != '\0') {
            info.title_id[sizeof(info.title_id) - 1] = '\0';

            if (!strstr(seen, info.title_id)) {
                if (seen[0] != '\0') {
                    strncat(seen, " ", seen_len - strlen(seen) - 1);
                }
                strncat(seen, info.title_id, seen_len - strlen(seen) - 1);
            }

            if (!strncmp(info.title_id, "PPSA", 4) || !strncmp(info.title_id, "CUSA", 4)) {
                found = 1;
            }
        }
        offset += (size_t)entry_size;
    }

    free(buf);
    return found;
}

int main(void) {
    int last_state = -1;
    int off_streak = 0;
    void *k_handle = NULL;

    // Signal-Handler registrieren
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Symbole dynamisch auflösen
    g_bd = (bd_fn)dlsym(RTLD_DEFAULT, "sceKernelIccControlBDPowerState");
    g_appinfo = (appinfo_fn)dlsym(RTLD_DEFAULT, "sceKernelGetAppInfo");
    g_notify = (notify_fn)dlsym(RTLD_DEFAULT, "sceKernelSendNotificationRequest");

    if (!g_bd || !g_appinfo || !g_notify) {
        k_handle = dlopen("libkernel.sprx", RTLD_LAZY);
        if (!k_handle) k_handle = dlopen("libkernel_web.sprx", RTLD_LAZY);
        if (k_handle) {
            if (!g_bd) g_bd = (bd_fn)dlsym(k_handle, "sceKernelIccControlBDPowerState");
            if (!g_appinfo) g_appinfo = (appinfo_fn)dlsym(k_handle, "sceKernelGetAppInfo");
            if (!g_notify) g_notify = (notify_fn)dlsym(k_handle, "sceKernelSendNotificationRequest");
        }
    }

    if (!g_bd || !g_appinfo) {
        logline(!g_bd ? "Error: sceKernelIccControlBDPowerState missing" : "Error: sceKernelGetAppInfo missing");
        if (k_handle) dlclose(k_handle);
        return 1;
    }

    char start_msg[128];
    snprintf(start_msg, sizeof(start_msg), "BD-Mute Daemon v%s initialized\nMade with ♥ by Akut0", BD_MUTE_VERSION);
    logline(start_msg);

    int current_poll_interval = IDLE_POLL_INTERVAL_SEC;

    while (g_running) {
        char seen[256];
        int is_game = game_running(seen, sizeof(seen));

        if (is_game) {
            off_streak = 0;
            current_poll_interval = GAME_POLL_INTERVAL_SEC;

            if (last_state != 1) {
                logline("BD-Mute: Game started -> BD off");
                set_bd(0);
                last_state = 1;
            }
        } else if (last_state == 1) {
            current_poll_interval = GAME_POLL_INTERVAL_SEC;

            if (++off_streak >= 2) {
                logline("BD-Mute: Game closed -> BD on");
                set_bd(1);
                last_state = 0;
                off_streak = 0;
                current_poll_interval = IDLE_POLL_INTERVAL_SEC;
            }
        } else if (last_state < 0) {
            last_state = 0;
            current_poll_interval = IDLE_POLL_INTERVAL_SEC;
        }

        sleep(current_poll_interval);
    }

    // Clean shutdown
    logline("BD-Mute: Shutting down daemon...");
    set_bd(1);

    if (k_handle) {
        dlclose(k_handle);
    }

    return 0;
}