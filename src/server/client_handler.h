#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H
void *client_thread(void *arg);   /* arg = malloc'd int* holding the socket fd */
#endif
