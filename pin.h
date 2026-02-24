#pragma once

#include <stdio.h>
#include <stdlib.h>

#define errExit(msg)    do {\
    perror(msg); \
    exit(EXIT_FAILURE); \
} while (0)

struct message {
    int type;
    int len;
    char data[0];
};

#define MSG_TYPE_DATA   1
#define MSG_TYPE_WSIZE  2
#define MSG_TYPE_EXIT   3
#define MSG_TYPE_ERROR  4
#define MSG_TYPE_ACCEPT 5

#define MAX_MSG_SIZE  4096
#define MAX_DATA_SIZE (MAX_MSG_SIZE - sizeof(struct message))


#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))


#define ESC "\033"

int attach(const char* path);
