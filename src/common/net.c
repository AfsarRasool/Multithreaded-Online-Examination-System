#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "net.h"
#include "common.h"

int net_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int net_accept(int listen_fd)
{
    return accept(listen_fd, NULL, NULL);
}

int net_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0 ||
        connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* TCP is a byte stream with no message boundaries, so we frame messages with '\n'. */
int send_line(int fd, const char *fmt, ...)
{
    char buf[MAX_LINE + 2];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, MAX_LINE, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n > MAX_LINE - 1) n = MAX_LINE - 1;
    buf[n++] = '\n';

    int sent = 0;
    while (sent < n) {
        int r = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) return -1;
        sent += r;
    }
    return 0;
}

/* Reads one byte at a time until '\n'. Simple and fine at this scale. */
int recv_line(int fd, char *buf, size_t size)
{
    size_t i = 0;
    while (i < size - 1) {
        char c;
        int r = recv(fd, &c, 1, 0);
        if (r == 0) return 0;          /* peer closed */
        if (r < 0)  return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return 1;
}
