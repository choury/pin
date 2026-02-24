#pragma once

// Send accept message and history to a new client
int server_send_history(int client_fd);

// Handle message from client, returns 0 normal, -1 disconnect
int server_handle_client(int client_fd, int pty_master);

// Read from pty_master, record history, and forward to client
// Returns bytes read, or -1 on error
int server_send_pty_data(int pty_master, int client_fd);


// Close all fds >= 3
void close_extra_fds(void);

// Create socket and daemonize, returns listen_fd
int server_start(const char* path, int foreground);
