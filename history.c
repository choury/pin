#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>

//环形缓冲区
struct cbuffer {
    char* buffer;
    size_t start;
    size_t len;
    size_t capacity;
};

void cbuffer_init(struct cbuffer* cb, size_t size) {
    cb->buffer = malloc(size);
    cb->capacity = size;
    cb->start = 0;
    cb->len = 0;
}

//写入数据，如果缓冲区满了，循环覆盖掉前面的数据, 返回实际写入的数据长度
int cbuffer_write(struct cbuffer* cb, const char* data, int len) {
    if (len <= 0) {
        return 0;
    }
    if (cb->len + len > cb->capacity) {
        //缓冲区满了
        int drop = cb->len + len - cb->capacity;
        if(drop > cb->len){
            //len 肯定比 capacity 大了，直接截取最后capacity长度的数据
            cb->start = 0;
            cb->len = 0;
            data = data + len - cb->capacity;
            len = cb->capacity;
        }else{
            //截取掉前面的部分数据
            cb->start = cb->start + drop;
            cb->len -= drop;
        }
    }
    size_t tail = (cb->start + cb->len) % cb->capacity;
    if (tail + len > cb->capacity) {
        //数据分两段存储
        size_t first = cb->capacity - tail;
        memcpy(cb->buffer + tail, data, first);
        memcpy(cb->buffer, data + first, len - first);
    } else {
        memcpy(cb->buffer + tail, data, len);
    }
    cb->len += len;
    assert(cb->len <= cb->capacity);
    return len;
}

// 写入单个字符,满了就挤掉最前面的
void cbuffer_push(struct cbuffer* cb, char c) {
    if (cb->len == cb->capacity) {
        cb->start = cb->start + 1;
        cb->len -= 1;
    }
    size_t tail = (cb->start + cb->len) % cb->capacity;
    cb->buffer[tail] = c;
    cb->len += 1;
}

//从off位置读取数据，返回实际读取的数据长度
int cbuffer_read(struct cbuffer* cb, char* data, off_t off, int len) {
    if (off + len > cb->len) {
        len = cb->len - off;
    }
    size_t front = (cb->start + off) % cb->capacity;
    if(front + len > cb->capacity){
        size_t first = cb->capacity - front;
        memcpy(data, cb->buffer + front, first);
        memcpy(data + first, cb->buffer, len - first);
    }else{
        memcpy(data, cb->buffer + front, len);
    }
    return len;
}

void cbuffer_free(struct cbuffer* cb) {
    free(cb->buffer);
    cb->buffer = NULL;
    cb->capacity = 0;
    cb->start = 0;
    cb->len = 0;
}

struct cbuffer main_buffer;
struct cbuffer alter_buffer;

void init_history() {
    cbuffer_init(&main_buffer, 1024 * 1024);
    cbuffer_init(&alter_buffer, 1024 * 1024);
}


int alter_screen = 0;

int mode = 0;
#define MODE_NORMAL   0
#define MODE_ESC      1
#define MODE_CSI      2
#define MODE_ZMODEM_1 3
#define MODE_ZMODEM_2 4
#define MODE_ZMODEM_3 5
#define MODE_ZMODEM   6

char csi_buffer[32];
size_t csi_len = 0;

void add_history(const char* data, size_t len) {
    //parse ansi escape code then add to cbuffer
    for(size_t i = 0; i < len ; i ++) {
        switch(mode & 0x00ff) {
        case MODE_NORMAL:
            if(data[i] == '\033') {
                mode = MODE_ESC;
            } else if(data[i] == 0x18) {
                mode = MODE_ZMODEM_1;
            } else {
                cbuffer_push(alter_screen?&alter_buffer:&main_buffer, data[i]);
            }
            break;
        case MODE_ESC:
            if(data[i] == '[') {
                mode = MODE_CSI;
                csi_buffer[0] = '\033';
                csi_buffer[1] = '[';
                csi_len = 2;
            } else {
                mode = MODE_NORMAL;
                cbuffer_push(alter_screen?&alter_buffer:&main_buffer, '\033');
                cbuffer_push(alter_screen?&alter_buffer:&main_buffer, data[i]);
            }
            break;
        case MODE_CSI:
            assert(csi_len >= 2);
            csi_buffer[csi_len++] = data[i];
            if(csi_len >= sizeof(csi_buffer)) {
                mode = MODE_NORMAL;
                cbuffer_write(alter_screen?&alter_buffer:&main_buffer, csi_buffer, csi_len);
                csi_len = 0;
                break;
            }
            if(data[i] >= 0x40 && data[i] <= 0x7e) {
                printf("finish csi mode: [%.*s]\n", csi_len-2, csi_buffer+2);
                mode = MODE_NORMAL;
                int enter_alt_screen = 0;
                if(memcmp(csi_buffer, "\033[?1049h", csi_len) == 0 || memcmp(csi_buffer, "\033[?47h", csi_len) == 0) {
                    enter_alt_screen = 1;
                    printf("enter alter screen\n");
                }else if(memcmp(csi_buffer, "\033[?1049l", csi_len) == 0 || memcmp(csi_buffer, "\033[?47l", csi_len) == 0) {
                    alter_screen = 0;
                    //clear alter screen
                    alter_buffer.len = 0;
                    alter_buffer.start = 0;
                    printf("leave alter screen\n");
                }else if(data[i] == 'c' ||
                  (data[i] == 'q' && memcmp(csi_buffer, "\033[>", 3) == 0) ||
                   memcmp(csi_buffer, "\033[6n", csi_len) == 0 ||
                   memcmp(csi_buffer, "\033[5n", csi_len) == 0) 
                {
                    //filter report sequence
                    csi_len = 0;
                    printf("filter report sequence\n");
                } else if(memcmp(csi_buffer, "\033[3J", csi_len) == 0) {
                    //clear screen
                    if(alter_screen) {
                        alter_buffer.len = 0;
                        alter_buffer.start = 0;
                    } else {
                        main_buffer.len = 0;
                        main_buffer.start = 0;
                    }
                    printf("clear screen\n");
                }
                cbuffer_write(alter_screen?&alter_buffer:&main_buffer, csi_buffer, csi_len);
                if(enter_alt_screen) {
                    alter_screen = 1;
                }
                csi_len = 0;
            }
            break;
        case MODE_ZMODEM_1:
            if (data[i] == 'B') {
                mode = MODE_ZMODEM_2;
            } else {
                mode = MODE_NORMAL;
                cbuffer_write(alter_screen?&alter_buffer:&main_buffer, "\x18""B", 2);
            }
            break;
        case MODE_ZMODEM_2:
            if (data[i] == '0') {
                mode = MODE_ZMODEM_3;
            } else {
                mode = MODE_NORMAL;
                cbuffer_write(alter_screen?&alter_buffer:&main_buffer, "\x18""B0", 3);
            }
            break;
        case MODE_ZMODEM_3:
            if (data[i] == '0' || data[i] == '1') {
                mode = MODE_ZMODEM;
                printf("enter zmodem mode\n");
            } else {
                mode = MODE_NORMAL;
                cbuffer_write(alter_screen?&alter_buffer:&main_buffer, "\x18""B0", 3);
                cbuffer_push(alter_screen?&alter_buffer:&main_buffer, data[i]);
            }
            break;
        case MODE_ZMODEM:
            if(data[i] == '\033') {
                mode = MODE_ESC;
                printf("leave zmodem mode\n");
            }
            break;
        }
    }
}

size_t history_len() {
    if(alter_screen) {
        return main_buffer.len + alter_buffer.len;
    }
    return main_buffer.len;
}

size_t history_read(char* data, off_t off, size_t len) {
    if(alter_screen && off >= main_buffer.len) {
        return cbuffer_read(&alter_buffer, data, off - main_buffer.len, len);
    }
    return cbuffer_read(&main_buffer, data, off, len);
}
