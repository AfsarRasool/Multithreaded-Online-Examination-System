#include <string.h>
#include "protocol.h"

int parse_message(char *line, Message *m)
{
    memset(m, 0, sizeof *m);
    if (line == NULL || line[0] == '\0') return -1;

    char *save = NULL;
    char *tok = strtok_r(line, "|", &save);
    m->cmd = tok;
    while ((tok = strtok_r(NULL, "|", &save)) != NULL && m->argc < MAX_ARGS)
        m->args[m->argc++] = tok;
    return 0;
}
