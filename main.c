/*
 * gcc main.c -lutil -o pin
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <pty.h>
#include <limits.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "pin.h"
#include "history.h"

struct init_args{
    int pty;
    char** argv;
    int argc;
    const char* socket;
};

static int init(void* data) {
    struct init_args *args = (struct init_args*)data;

    // Make the current process a new session leader
    setsid();

    dup2(args->pty, STDIN_FILENO);
    dup2(args->pty, STDOUT_FILENO);
    dup2(args->pty, STDERR_FILENO);

    // As the child is a session leader, set the controlling terminal to
    // be the slave side of the PTY (Mandatory for programs like the
    // shell to make them correctly manage their outputs)
    ioctl(0, TIOCSCTTY, 1);

    setenv("PIN_SOCK", args->socket, 1);
    if(execvpe(args->argv[0], args->argv, environ) <0 ){
        errExit("execve");
    }
    return 0;           /* Child terminates now */
}


static struct option long_options[] = {
    {"attach",    required_argument, 0, 'a'},
    {"foreground",no_argument,       0, 'f'},
    {"detach",    no_argument,       0,  0 },
    {0,           0,                 0,  0 }
};

int attach(const char* socket);


#define STACK_SIZE (1024 * 1024)    /* Stack size for cloned child */

int main(int argc, char *argv[]) {
    const char* attach_socket = NULL;
    int foreground = 0;
    int detach = 0;
    while (1) {
        int option_index = 0;

        int c = getopt_long(argc, argv, "a:f", long_options, &option_index);
        if (c == -1)
            break;
        if (c == '?'){
            fprintf(stderr, "Usage: %s [options...] command [args...]\n"
                            " -a/--attach <socket>\n"
                            " -f/--foreground\n" 
                            " --detach\n", argv[0]);
            return EXIT_FAILURE;
        }
        switch (c) {
        case 'a':
            printf("option -a/--attach with arg %s\n", optarg);
            attach_socket = optarg;
            break;
        case 'f':
            printf("option -f/--foreground\n");
            foreground = 1;
            detach = 1;
            break;
        case 0:
            if (strcmp(long_options[option_index].name, "detach") == 0) {
                printf("option --detach\n");
                detach = 1;
            }
            break;
        default:
            fprintf(stderr, "Unknown option: %c\n", c);
            return EXIT_FAILURE;
        }
    }
    if (getenv("PIN_SOCK")) {
        fprintf(stderr, "Can't execute pin inside pin session!\n");
        exit(-2);
    }
    if(attach_socket) {
        return attach(attach_socket);
    }
    struct init_args args = {};
    if(optind == argc){
        static char *default_argv[] = {NULL, NULL};
        default_argv[0] = getenv("SHELL");
        args.argc = 1;
        args.argv = default_argv;
    }else{
        args.argv = argv + optind;
        args.argc = argc - optind;
    }

    //put sock file into /tmp/pin-{uid}/
    //先确保改目录存在，不存在就创建一个
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/tmp/pin-%d", getuid());
    if(mkdir(path, 0700) < 0){
        if(errno != EEXIST){
            errExit("mkdir socket dir");
        }
    }

    //create unix socket {cmd}-{pid}.sock in socket dir
    sprintf(path + strlen(path), "/%s-%d.sock", basename(args.argv[0]), getpid());
    int cfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (cfd == -1) {
        errExit("create socket");
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(cfd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1) {
        errExit("bind socket");
    }
    if (listen(cfd, 5) == -1) {
        errExit("listen socket");
    }

    if(!detach && fork() > 0) {
        //close all fd
        for(int i = 3; i < 1024; i++){
            close(i);
        }
        wait(NULL);
        return attach(path);
    } else {
        printf("socket path: %s\n", path);
    }
    args.socket = path;

    if(!foreground) {
        daemon(1, 0);
    }
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 40;
    ws.ws_col = 80;

    int pty_master, pty_slave;
    // crate new pty
    if(openpty(&pty_master, &pty_slave, NULL, NULL, &ws) < 0){
        errExit("openpty");
    }

    int flags = fcntl(pty_master, F_GETFD);
    flags |= FD_CLOEXEC;
    fcntl(pty_master, F_SETFD, flags);

    flags = fcntl(pty_slave, F_GETFD);
    flags |= FD_CLOEXEC;
    fcntl(pty_slave, F_SETFD, flags);

    args.pty = pty_slave;

    /* Allocate stack for child */
    char *stack = malloc(STACK_SIZE);
    if (stack == NULL)
        errExit("malloc");

    pid_t pid = clone(init, stack + STACK_SIZE, SIGCHLD, &args);
    if (pid == -1)
        errExit("clone");
    printf("clone() returned %ld\n", (long) pid);

    int efd = epoll_create1(SOCK_CLOEXEC);
    struct  epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = pty_master;
    if(epoll_ctl(efd, EPOLL_CTL_ADD, pty_master, &ev)){
        errExit("epoll add pty");
    }

    ev.data.fd = cfd;
    if(epoll_ctl(efd, EPOLL_CTL_ADD, cfd, &ev)){
        errExit("epoll add socket");
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    /* Block signals so that they aren't handled
        according to their default dispositions. */
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1)
        errExit("sigprocmask");

    int sfd = signalfd(-1, &mask, SOCK_CLOEXEC);
    if (sfd == -1)
        errExit("signalfd");

    ev.data.fd = sfd;
    ev.events = EPOLLIN;
    if(epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ev)){
        errExit("epoll add signal");
    }


    int client_fd = -1;
    init_history();
    // copy date between stdin/stdout and childern's tty
    while(1){
        int n;
        struct  epoll_event evs[100];
        uint32_t timeout = -1;
        if((n = epoll_wait(efd, evs, sizeof(evs)/sizeof(struct epoll_event), timeout)) < 0){
            if(errno != EINTR){
                errExit("epoll_wait");
            }
            continue;
        }
        for(int i = 0; i< n; i++){
            int fd = evs[i].data.fd;
            if(fd == pty_master) {
                if(evs[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)){
                    goto ret;
                }
                char buff[MAX_MSG_SIZE];
                struct message* msg = (struct message*)buff;
                int ret = read(fd, msg->data, MAX_DATA_SIZE);
                if(ret <= 0){
                    goto ret;
                }
                add_history(msg->data, ret);
                msg->type = MSG_TYPE_DATA;
                msg->len = ret;
                if(client_fd > 0) {
                    write(client_fd, buff, sizeof(struct message) + ret);
                }
            }
            if(fd == client_fd){
                if(evs[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)){
                    close(client_fd);
                    client_fd = -1;
                    continue;
                }
                char buff[MAX_MSG_SIZE];
                int ret = read(fd, buff, sizeof(buff));
                if(ret <= 0){
                    goto ret;
                }
                struct message* msg = (struct message*)buff;
                if(msg->type == MSG_TYPE_DATA){
                    write(pty_master, msg->data, msg->len);
                }
                if(msg->type == MSG_TYPE_WSIZE){
                    struct winsize* ws = (struct winsize*)msg->data;
                    ioctl(pty_master, TIOCSWINSZ, ws);
                }
            }else if(fd == sfd){
                char buff[MAX_MSG_SIZE];
                struct message* msg = (struct message*)buff;
                ssize_t s = read(sfd, msg->data, sizeof(struct signalfd_siginfo));
                if (s != sizeof(struct signalfd_siginfo)) {
                    perror("read signalfd");
                    goto ret;
                }
                struct signalfd_siginfo* fdsi = (struct signalfd_siginfo*)msg->data;
                if (fdsi->ssi_signo == SIGCHLD) {
                    if(fdsi->ssi_pid != pid){
                        continue;
                    }
                    if(client_fd >= 0) {
                        msg->type = MSG_TYPE_EXIT;
                        msg->len = sizeof(struct signalfd_siginfo);
                        write(client_fd, buff, sizeof(struct message) + sizeof(struct signalfd_siginfo));
                    }
                    goto ret;
                }
                if ((fdsi->ssi_signo == SIGTERM) || (fdsi->ssi_signo == SIGINT)) {
                    //send signal to child
                    kill(pid, fdsi->ssi_signo);
                }
            } else if(fd == cfd){
                int nfd = accept(fd, NULL, NULL);
                if(nfd < 0){
                    perror("accept socket");
                    continue;
                }
                if(client_fd > 0){
                    //send error if one client already connected
                    struct message msg;
                    msg.type = MSG_TYPE_ERROR;
                    msg.len = 0;
                    write(nfd, &msg, sizeof(msg));
                    close(nfd);
                    continue;
                } else {
                    //send accept message
                    struct message msg;
                    msg.type = MSG_TYPE_ACCEPT;
                    msg.len = 0;
                    write(nfd, &msg, sizeof(msg));
                }
                client_fd = nfd;
                ev.data.fd = nfd;
                ev.events = EPOLLIN;
                if(epoll_ctl(efd, EPOLL_CTL_ADD, client_fd, &ev)){
                    perror("epoll add client");
                    close(client_fd);
                    client_fd = -1;
                    continue;
                }
                //发送历史记录
                int history = history_len();
                int history_left = history;
                char buff[MAX_MSG_SIZE];
                struct message* msg = (struct message*)buff;
                while(history_left > 0) {
                    int len = history_read(msg->data, history - history_left, MAX_DATA_SIZE);
                    msg->type = MSG_TYPE_DATA;
                    msg->len = len;
                    write(nfd, buff, sizeof(struct message) + len);
                    history_left -= len;
                }
                printf("sent history, size: %d\n", history);

            }
        }
    }
ret:
    close(pty_master);
    close(pty_slave);
    close(sfd);
    close(cfd);
    close(efd);
    unlink(path);
    return 0;
}
