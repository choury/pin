#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <fcntl.h>

#include "pin.h"


static void resize(const struct termios* old_termios_p){
    struct termios new;
    memcpy(&new, old_termios_p, sizeof(new));
	new.c_cflag |= (CLOCAL | CREAD);
	new.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	/* Users report:
	 *	The resize command messes up the terminal.
	 *	In my case it looks like it is hanging and
	 *	I need to press ctrl-c to get a prompt.
	 *	Actually the program does not hang but just
	 *	the terminal is messed up.
	 * Replaced TCSANOW with TCSAFLUSH:
	 * "the change occurs after all output written to fd
	 * has been transmitted, and all input that has been
	 * received but not read will be discarded before
	 * the change is made.
	 */
	tcsetattr(STDERR_FILENO, TCSAFLUSH, &new);

	/* save_cursor_pos 7
	 * scroll_whole_screen [r
	 * put_cursor_waaaay_off [$x;$yH
	 * get_cursor_pos [6n
	 * restore_cursor_pos 8
	 */ 
    struct winsize w = { 0, 0, 0, 0 };
	fprintf(stderr, ESC"7" ESC"[r" ESC"[999;999H" ESC"[6n");
    //BUG: death by signal won't restore termios
	scanf(ESC"[%hu;%huR", &w.ws_row, &w.ws_col);
	fprintf(stderr, ESC"8");

	/* BTW, other versions of resize recalculate w.ws_xpixel, ws.ws_ypixel
	 * by calculating character cell HxW from old values
	 * (gotten via TIOCGWINSZ) and recomputing *pixel values */
	ioctl(STDERR_FILENO, TIOCSWINSZ, &w);
    tcsetattr(STDERR_FILENO, TCSANOW, old_termios_p);
}

static int set_console(const struct termios* org_ops){
    if(isatty(STDOUT_FILENO) == 0){
        return 0;
    }
    struct termios term;
    memcpy(&term, org_ops, sizeof(struct termios));
    cfmakeraw(&term);
    if(tcsetattr(STDOUT_FILENO, TCSAFLUSH, &term)){
        perror("tcsetattr");
        return 1;
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
    return 0;
}



int attach(const char* path) {
    // connect to the unix domain socket
    if(!isatty(STDIN_FILENO)){
        fprintf(stderr, "stdin is not a tty\n");
        return 1;
    }

    struct termios org_ops;
    struct winsize ws;
    if(tcgetattr(STDOUT_FILENO, &org_ops)){
        errExit("tcgetattr");
    }
    resize(&org_ops);
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);

    int cfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (cfd == -1) {
        errExit("socket");
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(cfd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un)) == -1) {
        errExit("connect");
    }
    printf("connected to %s\n", path);
    //send initial window size
    char buf[MAX_MSG_SIZE];
    int n = read(cfd, buf, sizeof(buf));
    if (n < sizeof(struct message)) {
        fprintf(stderr, "failed to read accept\n");
        return -2;
    }
    struct message* msg = (struct message*)buf;
    if(msg->type != MSG_TYPE_ACCEPT) {
        fprintf(stderr, "failed to reuse connection: %d\n", msg->type);
        return -3;
    }
    //send size, ignore errors here
    msg->type = MSG_TYPE_WSIZE;
    msg->len = sizeof(struct winsize);
    memcpy(msg->data, &ws, sizeof(struct winsize));
    write(cfd, buf, sizeof(struct message) + sizeof(struct winsize));

    //epoll copy stdin to sfd and sfd to stdout
    int efd = epoll_create1(SOCK_CLOEXEC);
    if (efd == -1) {
        errExit("epoll_create1");
    }
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = STDIN_FILENO;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) == -1) {
        errExit("epoll add stdin");
    }
    ev.data.fd = cfd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, cfd, &ev) == -1) {
        errExit("epoll add cfd");
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGWINCH);

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

    int logfd = -1;
    //logfd = open("log", O_CREAT | O_WRONLY | O_TRUNC, 0644);

    struct signalfd_siginfo fdsi = {0};
    set_console(&org_ops);
    while (1) {
        struct epoll_event events[10];
        int n = epoll_wait(efd, events, 10, -1);
        if (n == -1) {
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; i++) {
            if (events[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                goto ret;
            }
            int fd = events[i].data.fd;
            if(fd == STDIN_FILENO) {
                char buf[MAX_MSG_SIZE];
                struct message* msg = (struct message*)buf;
                int n = read(fd, msg->data, MAX_DATA_SIZE);
                if (n == -1) {
                    perror("read from stdin");
                    continue;
                }
                if (n == 0) {
                    goto ret;
                }
                //check if input is ctrl+\ then exit
                if(n == 1 && msg->data[0] == 0x1c){
                    goto ret;
                }

                msg->type = MSG_TYPE_DATA;
                msg->len = n;
                write(cfd, buf, sizeof(struct message) + n);
            }
            if(fd == cfd){
                char buf[MAX_MSG_SIZE];
                int n = read(fd, buf, sizeof(buf));
                if (n == -1) {
                    perror("read from socket");
                    continue;
                }
                if (n == 0) {
                    goto ret;
                }
                struct message* msg = (struct message*)buf;
                if (msg->type == MSG_TYPE_DATA) {
                    write(STDOUT_FILENO, msg->data, msg->len);
                    //save to logfile for debug
                    if(logfd > 0){
                        write(logfd, msg->data, msg->len);
                    }
                }
                if (msg->type == MSG_TYPE_EXIT) {
                    memcpy(&fdsi, msg->data, sizeof(struct signalfd_siginfo));
                    goto ret;
                }
                if (msg->type == MSG_TYPE_ERROR) {
                    fprintf(stderr, "already attached");
                    goto ret;
                }
            }
            if (fd == sfd) {
                struct signalfd_siginfo fdsi;
                ssize_t s = read(sfd, &fdsi, sizeof(struct signalfd_siginfo));
                if (s != sizeof(struct signalfd_siginfo))
                    goto ret;
                if (fdsi.ssi_signo == SIGWINCH) {
                    struct winsize size;
                    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) < 0) {
                        perror("ioctl TIOCGWINSZ");
                        continue;
                    }
                    char buf[MAX_MSG_SIZE];
                    struct message* msg = (struct message*)buf;
                    msg->type = MSG_TYPE_WSIZE;
                    msg->len = sizeof(struct winsize);
                    memcpy(msg->data, &size, sizeof(struct winsize));
                    write(cfd, buf, sizeof(struct message) + sizeof(struct winsize));
                }
            }
        }
    }
ret:
    tcsetattr(STDOUT_FILENO, TCSANOW, &org_ops);
    close(sfd);
    close(cfd);
    close(efd);
    close(logfd);

    if (fdsi.ssi_code == CLD_EXITED) {
        printf("\nexited, code=%d\n", fdsi.ssi_status);
    } else if (fdsi.ssi_code == CLD_KILLED) {
        printf("\nkilled by signal %d\n", fdsi.ssi_status);
    } else if(fdsi.ssi_pid) {
        printf("\nsig code=%d,status=%d\n", fdsi.ssi_code, fdsi.ssi_status);
    } else {
        //printf(ESC"[?1004l"ESC"[?2004l"ESC"[?1049l"ESC"c\n");
        printf("Detached\n"ESC"[?1004l"ESC"[?2004l"ESC"[?1047l"ESC"[?1003l"ESC"[0J\n");
    }
    return fdsi.ssi_status;
}
