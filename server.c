#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <limits.h>

#include "pin.h"
#include "history.h"
#include "server.h"


void close_extra_fds(void) {
    close_range(3, ~0U, 0);
}


int server_send_history(int client_fd) {
    // send accept message
    struct message msg;
    msg.type = MSG_TYPE_ACCEPT;
    msg.len = 0;
    write(client_fd, &msg, sizeof(msg));

    // send history
    int history = history_len();
    int history_left = history;
    char buff[MAX_MSG_SIZE];
    struct message* hmsg = (struct message*)buff;
    while (history_left > 0) {
        int len = history_read(hmsg->data, history - history_left, MAX_DATA_SIZE);
        hmsg->type = MSG_TYPE_DATA;
        hmsg->len = len;
        write(client_fd, buff, sizeof(struct message) + len);
        history_left -= len;
    }
    printf("sent history, size: %d\n", history);
    return 0;
}

int server_handle_client(int client_fd, int pty_master) {
    char buff[MAX_MSG_SIZE];
    int ret = read(client_fd, buff, sizeof(buff));
    if (ret <= 0) {
        return -1;
    }
    struct message* msg = (struct message*)buff;
    if (msg->type == MSG_TYPE_DATA) {
        write(pty_master, msg->data, msg->len);
    }else if (msg->type == MSG_TYPE_WSIZE) {
        struct winsize* ws = (struct winsize*)msg->data;
        ioctl(pty_master, TIOCSWINSZ, ws);
    }
    return 0;
}

int server_send_pty_data(int pty_master, int client_fd) {
    char buff[MAX_MSG_SIZE];
    struct message* msg = (struct message*)buff;
    int len = read(pty_master, msg->data, MAX_DATA_SIZE);
    if (len <= 0) {
        return -1;
    }
    add_history(msg->data, len);
    if (client_fd >= 0) {
        msg->type = MSG_TYPE_DATA;
        msg->len = len;
        write(client_fd, buff, sizeof(struct message) + len);
    }
    return len;
}


int server_start(const char* path, int foreground) {
    int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listen_fd == -1) {
        errExit("create socket");
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1) {
        errExit("bind socket");
    }
    if (listen(listen_fd, 5) == -1) {
        errExit("listen socket");
    }
    printf("socket path: %s\n", path);

    // Become subreaper so we receive SIGCHLD for orphaned children
    // after daemon() re-parents us
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    if (!foreground) {
        daemon(1, 0);
    }
    init_history();
    return listen_fd;
}
