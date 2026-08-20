/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Browser-free dashboard entry for Retro Papa.
 *
 * RTPN00001 is both the visible Games title and the Sony BigApp host. The
 * title metadata contains no deeplink, so Shell launches the signed Sony host
 * directly. A resident watcher notices that genuine dashboard launch and
 * replaces it with the SDL2 frontend through the already-proven hbldr path.
 *
 * Retro Papa also uses hbldr when it launches an emulator. Those transitions
 * deliberately reuse RTPN00001, so explicit begin/end guards prevent process
 * churn created by Retro Papa itself from being mistaken for another user
 * click.
 */

#include "dashboard_watch.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "core/hbldr.h"

#define RP_TITLE_ID "RTPN00001"
#define RP_ROOT "/data/homebrew/RetroPapa"
#define RP_UI_PATH RP_ROOT "/retro-papa-ui.elf"
#define RP_STATE_DIR "/data/RetroPapa"
#define RP_LOG_PATH RP_STATE_DIR "/native-launcher.log"
#define RP_CHILD_LOG_PATH RP_STATE_DIR "/native-child.log"
#define RP_APPINST_AUTHID UINT64_C(0x4801000000000013)
#define RP_SETTLE_POLLS 80
#define RP_POLL_US 75000
#define RP_COPY_BUFFER_SIZE 65536U
#define RP_USER_APP_ROOT "/user/app"
#define RP_USER_TITLE_DIR RP_USER_APP_ROOT "/" RP_TITLE_ID
#define RP_USER_SCE_SYS RP_USER_TITLE_DIR "/sce_sys"
#define RP_USER_EBOOT RP_USER_TITLE_DIR "/eboot.bin"
#define RP_SONY_EBOOT "/system_ex/app/NPXS40106/eboot.bin"

#ifndef RP_BUILD_ID
#define RP_BUILD_ID "development"
#endif

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppUnInstall(const char *, void *, void *);
int sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);

static pthread_mutex_t rp_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rp_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int rp_watch_started = 0;
static int rp_internal_depth = 0;
static int rp_settle_polls = 0;

static void rp_watch_log(const char *message, long value) {
    char line[512];
    int length;
    int descriptor;

    if (!message) return;
    length = snprintf(
        line,
        sizeof(line),
        "%ld pid=%ld build=%s %s value=%ld\n",
        (long)time(NULL),
        (long)getpid(),
        RP_BUILD_ID,
        message,
        value);
    if (length <= 0) return;
    if ((size_t)length >= sizeof(line)) length = (int)sizeof(line) - 1;

    pthread_mutex_lock(&rp_log_mutex);
    descriptor = open(RP_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (descriptor >= 0) {
        (void)write(descriptor, line, (size_t)length);
        close(descriptor);
    }
    pthread_mutex_unlock(&rp_log_mutex);
}

static int rp_mkdir_checked(const char *path) {
    struct stat info;

    if (mkdir(path, 0755) == 0) return 0;
    if (errno != EEXIST) return -1;
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int rp_write_all(int descriptor, const uint8_t *data, size_t size) {
    size_t offset = 0;

    while (offset < size) {
        ssize_t written = write(descriptor, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (written == 0) {
            errno = ENOSPC;
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static int rp_stage_signed_eboot(void) {
    struct stat source_info;
    struct stat target_info;
    uint8_t *buffer = NULL;
    int source = -1;
    int target = -1;
    int result = -1;

    if (lstat(RP_SONY_EBOOT, &source_info) != 0 || !S_ISREG(source_info.st_mode) ||
        S_ISLNK(source_info.st_mode) || source_info.st_size < 4096) {
        errno = ENOEXEC;
        return -1;
    }

    if (lstat(RP_USER_EBOOT, &target_info) == 0 &&
        S_ISREG(target_info.st_mode) && !S_ISLNK(target_info.st_mode) &&
        target_info.st_size == source_info.st_size) {
        (void)chmod(RP_USER_EBOOT, 0755);
        return 0;
    }

    buffer = malloc(RP_COPY_BUFFER_SIZE);
    if (!buffer) {
        errno = ENOMEM;
        return -1;
    }
    source = open(RP_SONY_EBOOT, O_RDONLY | O_NOFOLLOW);
    if (source < 0) goto cleanup;
    target = open(RP_USER_EBOOT, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (target < 0) goto cleanup;

    result = 0;
    for (;;) {
        ssize_t got = read(source, buffer, RP_COPY_BUFFER_SIZE);
        if (got < 0) {
            if (errno == EINTR) continue;
            result = -1;
            break;
        }
        if (got == 0) break;
        if (rp_write_all(target, buffer, (size_t)got) != 0) {
            result = -1;
            break;
        }
    }
    if (result == 0 && fsync(target) != 0) result = -1;

cleanup:
    {
        int saved_errno = errno;
        if (target >= 0 && close(target) != 0 && result == 0) {
            result = -1;
            saved_errno = errno;
        }
        if (source >= 0) close(source);
        free(buffer);
        errno = saved_errno;
    }
    if (result == 0) (void)chmod(RP_USER_EBOOT, 0755);
    return result;
}

void rp_dashboard_internal_begin(void) {
    pthread_mutex_lock(&rp_state_mutex);
    if (rp_internal_depth < 1000) rp_internal_depth++;
    rp_settle_polls = 0;
    pthread_mutex_unlock(&rp_state_mutex);
}

void rp_dashboard_internal_end(void) {
    pthread_mutex_lock(&rp_state_mutex);
    if (rp_internal_depth > 0) rp_internal_depth--;
    if (rp_internal_depth == 0) rp_settle_polls = RP_SETTLE_POLLS;
    pthread_mutex_unlock(&rp_state_mutex);
}

int rp_prepare_real_title_registration(void) {
    pid_t pid = getpid();
    uint64_t original_authid = kernel_get_ucred_authid(pid);
    int init_result;
    int uninstall_result = -1;

    /* The previous test build registered RTPN00001 with a deeplink. Remove
     * that app.db entry first; launcher.c will recreate it from the new
     * browser-free param.json. */
    if (kernel_set_ucred_authid(pid, RP_APPINST_AUTHID) != 0) {
        rp_watch_log("real-title stale-uninstall authid-failed", -1);
        return -1;
    }

    init_result = sceAppInstUtilInitialize();
    if (init_result == 0) {
        uninstall_result = sceAppInstUtilAppUnInstall(RP_TITLE_ID, NULL, NULL);
        (void)sceAppInstUtilTerminate();
    }
    if (original_authid != 0) {
        (void)kernel_set_ucred_authid(pid, original_authid);
    }

    rp_watch_log("real-title stale-uninstall", (long)(uint32_t)uninstall_result);
    if (init_result != 0) {
        rp_watch_log("real-title AppInst init-failed", (long)(uint32_t)init_result);
        return -1;
    }
    usleep(250000);

    /* Stage the real signed eboot in the source directory passed to
     * sceAppInstUtilAppInstallTitleDir. This mirrors the validated folder-title
     * shape used by ps5upload (eboot.bin + sce_sys/param.json) instead of
     * relying on the old deeplink-only registration semantics. launcher.c
     * writes param.json/icon0.png into sce_sys immediately after this returns. */
    if (rp_mkdir_checked(RP_USER_APP_ROOT) != 0 ||
        rp_mkdir_checked(RP_USER_TITLE_DIR) != 0 ||
        rp_mkdir_checked(RP_USER_SCE_SYS) != 0 ||
        rp_stage_signed_eboot() != 0) {
        rp_watch_log("real-title signed-eboot staging-failed", errno);
        return -1;
    }
    rp_watch_log("real-title signed-eboot staged", 0);
    return 0;
}

static int rp_public_title_running(void) {
    uint32_t app_id = (uint32_t)sceLncUtilGetAppIdOfRunningBigApp();
    char title_id[16] = {0};

    if (app_id == UINT32_MAX) return 0;
    if (sceLncUtilGetAppTitleId(app_id, title_id) != 0) return 0;
    return strcmp(title_id, RP_TITLE_ID) == 0;
}

static int rp_launch_ui_from_dashboard(void) {
    char *argv[] = {(char *)RP_UI_PATH, NULL};
    char *envp[] = {NULL};
    int descriptor;
    pid_t child;

    descriptor = open(RP_CHILD_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0600);
    rp_watch_log("real-title dashboard-click detected", 0);

    rp_dashboard_internal_begin();
    child = hbldr_launch(RP_ROOT, RP_UI_PATH, descriptor, argv, envp);
    rp_dashboard_internal_end();

    if (descriptor >= 0) close(descriptor);
    rp_watch_log("real-title native-handoff pid", (long)child);
    return child > 0 ? 0 : -1;
}

static void *rp_dashboard_watch_thread(void *unused) {
    int armed = 1;
    (void)unused;

    rp_watch_log("real-title watcher active no-webkit", 0);
    for (;;) {
        int public_running = rp_public_title_running();
        int internal_depth;
        int settling;

        pthread_mutex_lock(&rp_state_mutex);
        internal_depth = rp_internal_depth;
        settling = rp_settle_polls;

        if (internal_depth > 0) {
            /* Do not mutate `armed` while hbldr owns a transition. */
            pthread_mutex_unlock(&rp_state_mutex);
            usleep(RP_POLL_US);
            continue;
        }

        if (settling > 0) {
            if (public_running) {
                /* The hbldr transition has reached the new RTPN00001 host.
                 * Keep the watcher disarmed until that app genuinely exits. */
                rp_settle_polls = 0;
                armed = 0;
            } else {
                rp_settle_polls--;
                if (rp_settle_polls == 0) armed = 1;
            }
            pthread_mutex_unlock(&rp_state_mutex);
            usleep(RP_POLL_US);
            continue;
        }
        pthread_mutex_unlock(&rp_state_mutex);

        if (public_running) {
            if (armed) {
                armed = 0;
                /* Let Shell finish creating the real title process before
                 * taking ownership of it through hbldr. */
                usleep(125000);
                (void)rp_launch_ui_from_dashboard();
            }
        } else {
            /* No RTPN00001 and no internal transition means the user is back
             * at the dashboard (or in another title), so a future click may
             * legitimately launch Retro Papa again. */
            armed = 1;
        }
        usleep(RP_POLL_US);
    }
    return NULL;
}

int rp_dashboard_watch_start(void) {
    pthread_t thread;
    int result;

    pthread_mutex_lock(&rp_state_mutex);
    if (rp_watch_started) {
        pthread_mutex_unlock(&rp_state_mutex);
        return 0;
    }
    rp_watch_started = 1;
    pthread_mutex_unlock(&rp_state_mutex);

    result = pthread_create(&thread, NULL, rp_dashboard_watch_thread, NULL);
    if (result != 0) {
        pthread_mutex_lock(&rp_state_mutex);
        rp_watch_started = 0;
        pthread_mutex_unlock(&rp_state_mutex);
        rp_watch_log("real-title watcher pthread-create-failed", result);
        return -1;
    }
    result = pthread_detach(thread);
    if (result != 0) {
        rp_watch_log("real-title watcher pthread-detach-failed", result);
        return -1;
    }
    rp_watch_log("real-title watcher started", 0);
    return 0;
}
