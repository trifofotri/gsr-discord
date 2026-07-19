#include "../include/wayland_host_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <wayland-client.h>

#define GSR_UI_UNIX_SOCKET_DOMAIN_FD 3

static int unescape_key_file_value(char *value) {
    char *source = value;
    char *destination = value;
    while(*source) {
        if(*source != '\\') {
            *destination++ = *source++;
            continue;
        }

        ++source;
        switch(*source) {
            case 's': *destination++ = ' '; break;
            case 'n': *destination++ = '\n'; break;
            case 't': *destination++ = '\t'; break;
            case 'r': *destination++ = '\r'; break;
            case '\\': *destination++ = '\\'; break;
            default: return 0;
        }
        ++source;
    }
    *destination = '\0';
    return 1;
}

static int get_bridge_host_path(char *path, size_t path_size) {
    FILE *flatpak_info = fopen("/.flatpak-info", "r");
    if(!flatpak_info)
        return 0;

    char line[PATH_MAX];
    char app_path[PATH_MAX] = {0};
    int in_instance_group = 0;
    int result = 0;
    while(fgets(line, sizeof(line), flatpak_info)) {
        if(!strchr(line, '\n') && !feof(flatpak_info))
            break;
        line[strcspn(line, "\r\n")] = '\0';

        if(line[0] == '[') {
            in_instance_group = strcmp(line, "[Instance]") == 0;
            continue;
        }
        if(!in_instance_group)
            continue;

        const char prefix[] = "app-path=";
        if(strncmp(line, prefix, sizeof(prefix) - 1) != 0)
            continue;

        snprintf(app_path, sizeof(app_path), "%s", line + sizeof(prefix) - 1);
    }

    fclose(flatpak_info);
    if(!unescape_key_file_value(app_path) || app_path[0] != '/')
        return 0;

    const int written = snprintf(path, path_size, "%s/bin/gsr-wayland-bridge", app_path);
    result = written > 0 && (size_t)written < path_size;
    return result;
}

static int recv_fd(int sock) {
    char dummy = 0;
    struct iovec iov = { &dummy, 1 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    const ssize_t n = recvmsg(sock, &msg, 0);
    if(n <= 0)
        return -1;

    for(struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if(c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int fd = -1;
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
            return fd;
        }
    }
    return -1;
}

static struct wl_display* connect_via_bridge() {
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    char bridge_host_path[PATH_MAX];
    if(!get_bridge_host_path(bridge_host_path, sizeof(bridge_host_path))) {
        fprintf(stderr, "WaylandHostBridge: failed to resolve the Flatpak app path\n");
        return NULL;
    }

    int sv[2];
    if(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        perror("WaylandHostBridge: socketpair");
        return NULL;
    }

    const pid_t pid = fork();
    if(pid < 0) {
        perror("WaylandHostBridge: fork");
        close(sv[0]);
        close(sv[1]);
        return NULL;
    }

    if(pid == 0) {
        close(sv[0]);
        if(sv[1] != GSR_UI_UNIX_SOCKET_DOMAIN_FD) {
            dup2(sv[1], GSR_UI_UNIX_SOCKET_DOMAIN_FD);
            close(sv[1]);
        }
        const int flags = fcntl(GSR_UI_UNIX_SOCKET_DOMAIN_FD, F_GETFD);
        if(flags >= 0)
            fcntl(GSR_UI_UNIX_SOCKET_DOMAIN_FD, F_SETFD, flags & ~FD_CLOEXEC);

        char forward_fd_arg[32];
        snprintf(forward_fd_arg, sizeof(forward_fd_arg), "--forward-fd=%d", GSR_UI_UNIX_SOCKET_DOMAIN_FD);

        execlp("flatpak-spawn", "flatpak-spawn", "--host", forward_fd_arg, bridge_host_path, wayland_display, (char*)NULL);
        _exit(127);
    }

    close(sv[1]);
    const int wayland_fd = recv_fd(sv[0]);
    close(sv[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    if(wayland_fd < 0) {
        fprintf(stderr, "WaylandHostBridge: gsr-wayland-bridge did not return a wayland fd\n");
        return NULL;
    }

    struct wl_display *dpy = wl_display_connect_to_fd(wayland_fd);
    if(!dpy) {
        close(wayland_fd);
        fprintf(stderr, "WaylandHostBridge: wl_display_connect_to_fd failed\n");
        return NULL;
    }
    return dpy;
}

struct wl_display* wayland_connect_to_host() {
    const int inside_flatpak = getenv("FLATPAK_ID") != NULL;
    if(inside_flatpak) {
        struct wl_display *dpy = connect_via_bridge();
        if(dpy)
            return dpy;
        fprintf(stderr, "WaylandHostBridge: falling back to sandboxed wl_display_connect\n");
    }
    return wl_display_connect(NULL);
}
