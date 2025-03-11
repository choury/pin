#include <stddef.h>
#include <sys/types.h>

void init_history();
void add_history(const char* data, size_t len);
size_t history_len();
size_t history_read(char* data, off_t off, size_t len);