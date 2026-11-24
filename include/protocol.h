#ifndef PROTOCOL_H
#define PROTOCOL_H
#include "common.h"

/* A message looks like:  COMMAND|arg1|arg2|...   */
typedef struct {
    char *cmd;
    char *args[MAX_ARGS];
    int   argc;
} Message;

/* Splits 'line' IN PLACE (it modifies the buffer). Returns 0 on success. */
int parse_message(char *line, Message *m);

#endif
