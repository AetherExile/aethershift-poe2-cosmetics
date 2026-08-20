/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Retro Papa resident PS5 launcher.
 *
 * The payload installs a normal Games tile, owns a loopback-only launch
 * endpoint, and uses the retained Sony BigApp transition/ELF replacement core
 * to start the SDL2 frontend and configured emulators. It does not start,
 * embed, or contact ps5-payload-websrv during normal use.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#include "core/hbldr.h"
#include "core/standalone_fs.h"
#include "route.h"

#define RP_TITLE_ID "RTPN00001"
#define RP_SERVICE_ADDRESS "127.0.0.1"
#define RP_SERVICE_PORT 9041
#define RP_ROOT "/data/homebrew/RetroPapa"
#define RP_UI_PATH RP_ROOT "/retro-papa-ui.elf"
#define RP_PAYLOAD_PATH RP_ROOT "/retro-papa-native.elf"
#define RP_STATE_DIR "/data/RetroPapa"
#define RP_LOG_PATH RP_STATE_DIR "/native-launcher.log"
#define RP_CHILD_LOG_PATH RP_STATE_DIR "/native-child.log"
#define RP_LOCK_PATH RP_STATE_DIR "/native-launcher.lock"
#define RP_APP_ROOT "/user/app"
#define RP_APP_PARENT RP_APP_ROOT "/"
#define RP_APP_DIR RP_APP_ROOT "/" RP_TITLE_ID
#define RP_APPINST_AUTHID UINT64_C(0x4801000000000013)
#define RP_REQUEST_CAP 8192

#ifndef RP_BUILD_ID
#define RP_BUILD_ID "development"
#endif

#define INCASSET(name, file)                                                   \
    __asm__(".section .rodata\n"                                               \
            ".global " #name "\n" #name ":\n"                                \
            ".incbin \"" file "\"\n" #name "_end:\n"                         \
            ".global " #name "_size\n" #name "_size:\n"                      \
            ".quad " #name "_end - " #name "\n");                            \
    extern const uint8_t name[];                                               \
    extern const unsigned long name##_size

INCASSET(rp_embedded_ui, "../app/retro-papa-ui.elf");
INCASSET(rp_tile_param, "assets/param.json");
INCASSET(rp_icon_png, "../app/icon0.png");

typedef int (*app_install_title_dir_fn)(
    const char *title_id,
    const char *app_root,
    void *reserved);

typedef struct rp_notify_request {
    char reserved[45];
    char message[3075];
} rp_notify_request_t;

int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *, void *, void *);
int sceKernelSendNotificationRequest(int, void *, size_t, int);

static int mkdir_if_needed(const char *path) {
    struct stat info;

    if (mkdir(path, 0755) == 0) {
        return 0;
    }
    if (errno != EEXIST) {
        return -errno;
    }
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) {
        return -ENOTDIR;
    }
    return 0;
}

static void launcher_log(const char *format, ...) {
    char body[1024];
    char line[1280];
    va_list args;
    int length;
    int descriptor;

    va_start(args, format);
    vsnprintf(body, sizeof(body), format ? format : "", args);
    va_end(args);
    length = snprintf(
        line,
        sizeof(line),
        "%ld pid=%ld build=%s %s\n",
        (long)time(NULL),
        (long)getpid(),
        RP_BUILD_ID,
        body);
    if (length <= 0) {
        return;
    }
    if ((size_t)length >= sizeof(line)) {
        length = (int)sizeof(line) - 1;
    }
    fputs(line, stdout);
    fflush(stdout);
    (void)mkdir_if_needed("/data");
    (void)mkdir_if_needed(RP_STATE_DIR);
    descriptor = open(RP_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (descriptor >= 0) {
        (void)write(descriptor, line, (size_t)length);
        close(descriptor);
    }
}

static void notify_ready(void) {
    rp_notify_request_t request;
    int result;

    memset(&request, 0, sizeof(request));
    snprintf(
        request.message,
        sizeof(request.message),
        "Retro Papa listo. Abre Retro Papa desde Juegos.");
    result = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    launcher_log("ready notification rc=0x%08x", (unsigned int)result);
}

static int write_all(int descriptor, const uint8_t *data, size_t size) {
    size_t offset = 0;

    while (offset < size) {
        ssize_t written = write(descriptor, data + offset, size - offset);
        if (written <= 0) {
            errno = written == 0 ? ENOSPC : errno;
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static int write_file_atomic(
    const char *path,
    const uint8_t *data,
    size_t size,
    mode_t mode) {
    char temporary[640];
    int descriptor;
    int result = 0;

    if (!path || !data || snprintf(
            temporary,
            sizeof(temporary),
            "%s.tmp.%ld",
            path,
            (long)getpid()) >= (int)sizeof(temporary)) {
        return -EINVAL;
    }
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (descriptor < 0) {
        return -errno;
    }
    if (write_all(descriptor, data, size) != 0 || fsync(descriptor) != 0) {
        result = -errno;
    }
    if (close(descriptor) != 0 && result == 0) {
        result = -errno;
    }
    if (result != 0) {
        (void)unlink(temporary);
        return result;
    }
    if (rename(temporary, path) != 0) {
        result = -errno;
        (void)unlink(temporary);
        return result;
    }
    (void)chmod(path, mode);
    return 0;
}

static int regular_file_matches(
    const char *path,
    const uint8_t *expected,
    size_t expected_size) {
    uint8_t buffer[16384];
    struct stat path_info;
    struct stat opened_info;
    size_t offset = 0;
    int descriptor;

    if (lstat(path, &path_info) != 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (!S_ISREG(path_info.st_mode) || S_ISLNK(path_info.st_mode) ||
        (uintmax_t)path_info.st_size != (uintmax_t)expected_size) {
        return 0;
    }
    descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) {
        return -errno;
    }
    if (fstat(descriptor, &opened_info) != 0 ||
        !S_ISREG(opened_info.st_mode) ||
        opened_info.st_dev != path_info.st_dev ||
        opened_info.st_ino != path_info.st_ino ||
        opened_info.st_size != path_info.st_size) {
        int error = errno ? errno : ESTALE;
        close(descriptor);
        return -error;
    }
    while (offset < expected_size) {
        size_t remaining = expected_size - offset;
        size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t got = read(descriptor, buffer, requested);
        if (got <= 0) {
            int error = got == 0 ? EIO : errno;
            close(descriptor);
            return -error;
        }
        if (memcmp(expected + offset, buffer, (size_t)got) != 0) {
            close(descriptor);
            return 0;
        }
        offset += (size_t)got;
    }
    if (close(descriptor) != 0) {
        return -errno;
    }
    return 1;
}

static int update_file_atomic(
    const char *path,
    const uint8_t *data,
    size_t size,
    mode_t mode,
    int *changed) {
    int matches = regular_file_matches(path, data, size);
    int result;

    if (matches < 0) {
        return matches;
    }
    if (matches > 0) {
        (void)chmod(path, mode);
        if (changed) *changed = 0;
        return 0;
    }
    result = write_file_atomic(path, data, size, mode);
    if (result == 0 && changed) *changed = 1;
    return result;
}

uint8_t *fs_readfile(const char *path, size_t *size) {
    struct stat path_info;
    struct stat opened_info;
    uint8_t *data;
    size_t offset = 0;
    int descriptor;

    if (!path || lstat(path, &path_info) != 0 ||
        !S_ISREG(path_info.st_mode) || S_ISLNK(path_info.st_mode) ||
        path_info.st_size <= 0 ||
        (uintmax_t)path_info.st_size > (uintmax_t)SIZE_MAX - 1U) {
        return NULL;
    }
    descriptor = open(path, O_RDONLY | O_NOFOLLOW);
    if (descriptor < 0) {
        return NULL;
    }
    if (fstat(descriptor, &opened_info) != 0 ||
        !S_ISREG(opened_info.st_mode) ||
        opened_info.st_dev != path_info.st_dev ||
        opened_info.st_ino != path_info.st_ino ||
        opened_info.st_size != path_info.st_size) {
        close(descriptor);
        return NULL;
    }
    data = malloc((size_t)path_info.st_size + 1U);
    if (!data) {
        close(descriptor);
        return NULL;
    }
    while (offset < (size_t)path_info.st_size) {
        ssize_t got = read(descriptor, data + offset, (size_t)path_info.st_size - offset);
        if (got <= 0) {
            int error = got == 0 ? EIO : errno;
            free(data);
            close(descriptor);
            errno = error;
            return NULL;
        }
        offset += (size_t)got;
    }
    close(descriptor);
    data[offset] = 0;
    if (size) *size = offset;
    return data;
}

static int install_embedded_ui(void) {
    int changed = 0;
    int result;

    if (rp_embedded_ui_size < 5 ||
        rp_embedded_ui[0] != 0x7f || rp_embedded_ui[1] != 'E' ||
        rp_embedded_ui[2] != 'L' || rp_embedded_ui[3] != 'F' ||
        rp_embedded_ui[4] != 2) {
        return -ENOEXEC;
    }
    if ((result = mkdir_if_needed("/data")) != 0 ||
        (result = mkdir_if_needed("/data/homebrew")) != 0 ||
        (result = mkdir_if_needed(RP_ROOT)) != 0 ||
        (result = mkdir_if_needed(RP_STATE_DIR)) != 0) {
        return result;
    }
    result = update_file_atomic(
        RP_UI_PATH,
        rp_embedded_ui,
        (size_t)rp_embedded_ui_size,
        0755,
        &changed);
    launcher_log(
        "ui install rc=%d changed=%d bytes=%lu",
        result,
        changed,
        rp_embedded_ui_size);
    return result;
}

static void remove_path_if_present(const char *path) {
    if (unlink(path) != 0 && errno != ENOENT) {
        launcher_log("legacy file remove failed path=%s errno=%d", path, errno);
    }
}

static void remove_legacy_registration(const char *title_id) {
    char base[256];
    char sce_sys[320];
    char path[384];
    int result;

    result = sceAppInstUtilAppUnInstall(title_id, NULL, NULL);
    launcher_log("legacy uninstall title=%s rc=0x%08x", title_id, result);
    snprintf(base, sizeof(base), RP_APP_ROOT "/%s", title_id);
    snprintf(sce_sys, sizeof(sce_sys), "%s/sce_sys", base);
    snprintf(path, sizeof(path), "%s/launch.html", base);
    remove_path_if_present(path);
    snprintf(path, sizeof(path), "%s/param.json", sce_sys);
    remove_path_if_present(path);
    snprintf(path, sizeof(path), "%s/icon0.png", sce_sys);
    remove_path_if_present(path);
    (void)rmdir(sce_sys);
    (void)rmdir(base);
}

static int install_dashboard_tile(void) {
    char sce_sys[320];
    char param_path[384];
    char icon_path[384];
    uint32_t appinst_handle = 0;
    app_install_title_dir_fn install_title_dir = NULL;
    pid_t pid = getpid();
    uint64_t original_authid = kernel_get_ucred_authid(pid);
    int changed;
    int title_dir_result = -1;
    int install_all_result = -1;
    int result = -1;

    if (kernel_set_ucred_authid(pid, RP_APPINST_AUTHID) != 0) {
        launcher_log("AppInst authid elevation failed");
        return -1;
    }
    if (sceAppInstUtilInitialize() != 0) {
        launcher_log("sceAppInstUtilInitialize failed");
        goto cleanup;
    }

    remove_legacy_registration("RTPA00001");
    remove_legacy_registration("RETP00001");
    usleep(300000);

    snprintf(sce_sys, sizeof(sce_sys), "%s/sce_sys", RP_APP_DIR);
    snprintf(param_path, sizeof(param_path), "%s/param.json", sce_sys);
    snprintf(icon_path, sizeof(icon_path), "%s/icon0.png", sce_sys);
    if (mkdir_if_needed(RP_APP_DIR) != 0 || mkdir_if_needed(sce_sys) != 0) {
        launcher_log("dashboard directories could not be created");
        goto terminate;
    }
    if (update_file_atomic(
            param_path,
            rp_tile_param,
            (size_t)rp_tile_param_size,
            0644,
            &changed) != 0) {
        launcher_log("dashboard param write failed");
        goto terminate;
    }
    if (update_file_atomic(
            icon_path,
            rp_icon_png,
            (size_t)rp_icon_png_size,
            0644,
            &changed) != 0) {
        launcher_log("dashboard icon write failed");
        goto terminate;
    }

    if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &appinst_handle) == 0) {
        install_title_dir = (app_install_title_dir_fn)kernel_dynlib_resolve(
            -1,
            appinst_handle,
            "Wudg3Xe3heE");
    }
    if (install_title_dir) {
        title_dir_result = install_title_dir(RP_TITLE_ID, RP_APP_PARENT, NULL);
    }
    if (title_dir_result != 0) {
        install_all_result = sceAppInstUtilAppInstallAll(NULL);
    }
    if (title_dir_result == 0 || install_all_result == 0) {
        result = 0;
    }
    launcher_log(
        "dashboard install result=%d title_dir=0x%08x install_all=0x%08x",
        result,
        title_dir_result,
        install_all_result);

terminate:
    (void)sceAppInstUtilTerminate();
cleanup:
    if (original_authid != 0) {
        (void)kernel_set_ucred_authid(pid, original_authid);
    }
    return result;
}

static int file_is_regular(const char *path) {
    struct stat info;
    return path && lstat(path, &info) == 0 && S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode);
}

static int open_child_log(void) {
    (void)mkdir_if_needed(RP_STATE_DIR);
    return open(RP_CHILD_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0600);
}

static int launch_ui(void) {
    char *argv[] = {(char *)RP_UI_PATH, NULL};
    char *envp[] = {NULL};
    int stdio_descriptor;
    pid_t child;

    if (install_embedded_ui() != 0 || !file_is_regular(RP_UI_PATH)) {
        launcher_log("UI unavailable");
        return -1;
    }
    stdio_descriptor = open_child_log();
    launcher_log("launch UI begin path=%s", RP_UI_PATH);
    child = hbldr_launch(RP_ROOT, RP_UI_PATH, stdio_descriptor, argv, envp);
    if (stdio_descriptor >= 0) close(stdio_descriptor);
    launcher_log("launch UI result pid=%ld", (long)child);
    return child > 0 ? 0 : -1;
}

static int launch_homebrew(const rp_launch_request_t *request) {
    char *argv[3];
    char *envp[2];
    int stdio_descriptor;
    pid_t child;

    if (!request || !file_is_regular(request->path)) {
        launcher_log("homebrew path is missing or not regular");
        return -1;
    }
    argv[0] = (char *)request->path;
    argv[1] = request->args[0] ? (char *)request->args : NULL;
    argv[2] = NULL;
    envp[0] = request->env[0] ? (char *)request->env : NULL;
    envp[1] = NULL;
    stdio_descriptor = open_child_log();
    launcher_log(
        "launch homebrew begin path=%s cwd=%s args=%s env=%s",
        request->path,
        request->cwd,
        request->args,
        request->env);
    child = hbldr_launch(
        request->cwd,
        request->path,
        stdio_descriptor,
        argv,
        envp);
    if (stdio_descriptor >= 0) close(stdio_descriptor);
    launcher_log("launch homebrew result pid=%ld", (long)child);
    return child > 0 ? 0 : -1;
}

static int create_loopback_listener(void) {
    struct sockaddr_in address;
    int listener;
    int enabled = 1;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return -1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(RP_SERVICE_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 4) != 0) {
        int error = errno;
        close(listener);
        errno = error;
        return -1;
    }
    return listener;
}

static int send_all_text(int connection, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t sent = send(connection, data + offset, size - offset, 0);
        if (sent <= 0) return -1;
        offset += (size_t)sent;
    }
    return 0;
}

static void send_response(int connection, int status, const char *body) {
    char header[512];
    const char *reason =
        status == 200 ? "OK" :
        status == 204 ? "No Content" :
        status == 400 ? "Bad Request" :
        status == 500 ? "Internal Server Error" :
        "Not Found";
    size_t body_length = body ? strlen(body) : 0;
    int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        status,
        reason,
        body_length);

    if (header_length > 0 && header_length < (int)sizeof(header)) {
        (void)send_all_text(connection, header, (size_t)header_length);
    }
    if (body_length) {
        (void)send_all_text(connection, body, body_length);
    }
}

static ssize_t receive_request(int connection, char *buffer, size_t cap) {
    size_t used = 0;
    struct timeval timeout = {2, 0};

    (void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    while (used + 1 < cap) {
        ssize_t got = recv(connection, buffer + used, cap - used - 1, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        used += (size_t)got;
        buffer[used] = '\0';
        if (strstr(buffer, "\r\n\r\n")) break;
    }
    buffer[used] = '\0';
    return (ssize_t)used;
}

static int request_existing_shutdown(void) {
    static const char request[] =
        "GET /shutdown HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n";
    struct sockaddr_in address;
    char response[128];
    struct timeval timeout = {1, 0};
    int connection = socket(AF_INET, SOCK_STREAM, 0);
    ssize_t got;

    if (connection < 0) return -1;
    (void)setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(RP_SERVICE_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(connection, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        send_all_text(connection, request, sizeof(request) - 1U) != 0) {
        close(connection);
        return -1;
    }
    got = recv(connection, response, sizeof(response) - 1U, 0);
    close(connection);
    if (got <= 0) return -1;
    response[got] = '\0';
    return strstr(response, "HTTP/1.1 200 ") == response ? 0 : -1;
}

static int acquire_instance_lock(void) {
    char owner[128];
    int descriptor;
    int attempt;
    int length;

    if (mkdir_if_needed("/data") != 0 || mkdir_if_needed(RP_STATE_DIR) != 0) {
        return -1;
    }
    descriptor = open(RP_LOCK_PATH, O_RDWR | O_CREAT, 0600);
    if (descriptor < 0) return -1;
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            close(descriptor);
            return -1;
        }
        launcher_log("existing launcher detected; requesting takeover");
        if (request_existing_shutdown() != 0) {
            close(descriptor);
            errno = EADDRINUSE;
            return -1;
        }
        for (attempt = 0; attempt < 75; ++attempt) {
            usleep(40000);
            if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) break;
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                close(descriptor);
                return -1;
            }
        }
        if (attempt == 75) {
            close(descriptor);
            errno = EADDRINUSE;
            return -1;
        }
    }
    length = snprintf(owner, sizeof(owner), "pid=%ld build=%s\n", (long)getpid(), RP_BUILD_ID);
    if (length > 0 && length < (int)sizeof(owner) &&
        ftruncate(descriptor, 0) == 0 && lseek(descriptor, 0, SEEK_SET) == 0) {
        (void)write(descriptor, owner, (size_t)length);
        (void)fsync(descriptor);
    }
    return descriptor;
}

static int create_listener_with_retry(void) {
    int listener;
    int attempt;

    for (attempt = 0; attempt < 75; ++attempt) {
        listener = create_loopback_listener();
        if (listener >= 0) return listener;
        if (errno != EADDRINUSE) return -1;
        if (attempt == 0) (void)request_existing_shutdown();
        usleep(40000);
    }
    errno = EADDRINUSE;
    return -1;
}

static void serve_forever(int listener) {
    for (;;) {
        char request[RP_REQUEST_CAP];
        rp_launch_request_t launch_request;
        int connection = accept(listener, NULL, NULL);
        ssize_t received;

        if (connection < 0) {
            if (errno == EINTR) continue;
            launcher_log("accept failed errno=%d", errno);
            continue;
        }
#ifdef SO_NOSIGPIPE
        {
            int enabled = 1;
            (void)setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
        }
#endif
        received = receive_request(connection, request, sizeof(request));
        if (received <= 0) {
            send_response(connection, 400, "Empty request\n");
            close(connection);
            continue;
        }

        if (rp_request_path_is(request, "/launch")) {
            launcher_log("request route=/launch");
            if (launch_ui() == 0) send_response(connection, 204, "");
            else send_response(connection, 500, "Retro Papa could not be started\n");
        } else if (rp_request_path_is(request, "/hbldr")) {
            launcher_log("request route=/hbldr");
            if (rp_parse_hbldr_request(request, &launch_request) != 0) {
                send_response(connection, 400, "Invalid launch request\n");
            } else if (launch_homebrew(&launch_request) == 0) {
                send_response(connection, 204, "");
            } else {
                send_response(connection, 500, "Homebrew could not be started\n");
            }
        } else if (rp_request_path_is(request, "/status")) {
            send_response(connection, 200, "Retro Papa native launcher ready\n");
        } else if (rp_request_path_is(request, "/shutdown")) {
            send_response(connection, 200, "Shutting down\n");
            close(connection);
            launcher_log("request route=/shutdown action=exit");
            return;
        } else {
            send_response(connection, 404, "Not found\n");
        }
        close(connection);
    }
}

int main(void) {
    int instance_lock;
    int listener;
    int result;

    (void)signal(SIGPIPE, SIG_IGN);
    launcher_log(
        "start address=%s port=%d payload=%s ui_bytes=%lu",
        RP_SERVICE_ADDRESS,
        RP_SERVICE_PORT,
        RP_PAYLOAD_PATH,
        rp_embedded_ui_size);

    instance_lock = acquire_instance_lock();
    if (instance_lock < 0) {
        launcher_log("instance ownership failed errno=%d", errno);
        return 10;
    }
    if ((result = install_embedded_ui()) != 0) {
        launcher_log("embedded UI install failed rc=%d", result);
        close(instance_lock);
        return 11;
    }
    if (sceUserServiceInitialize(NULL) != 0) {
        launcher_log("UserService initialization failed");
        close(instance_lock);
        return 12;
    }
    if (hbldr_prepare_host() != 0) {
        launcher_log("Sony BigApp host preparation failed errno=%d", errno);
        (void)sceUserServiceTerminate();
        close(instance_lock);
        return 13;
    }
    if (install_dashboard_tile() != 0) {
        launcher_log("dashboard tile installation failed");
        (void)sceUserServiceTerminate();
        close(instance_lock);
        return 14;
    }
    listener = create_listener_with_retry();
    if (listener < 0) {
        launcher_log("loopback listener failed errno=%d", errno);
        (void)sceUserServiceTerminate();
        close(instance_lock);
        return 15;
    }

    launcher_log(
        "ready address=%s port=%d routes=/launch,/hbldr,/status,/shutdown websrv=unused title=%s",
        RP_SERVICE_ADDRESS,
        RP_SERVICE_PORT,
        RP_TITLE_ID);
    notify_ready();
    serve_forever(listener);

    close(listener);
    (void)flock(instance_lock, LOCK_UN);
    close(instance_lock);
    (void)sceUserServiceTerminate();
    payload_exit(0);
    return 0;
}
