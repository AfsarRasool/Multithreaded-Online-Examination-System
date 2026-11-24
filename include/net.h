#ifndef NET_H
#define NET_H
#include <stddef.h>

int net_listen(int port);               /* returns listening fd or -1 */
int net_accept(int listen_fd);          /* returns client fd or -1 */
int net_connect(const char *host, int port);
int send_line(int fd, const char *fmt, ...);        /* appends '\n' */
int recv_line(int fd, char *buf, size_t size);      /* >0 ok, 0 closed, -1 error */

#endif
