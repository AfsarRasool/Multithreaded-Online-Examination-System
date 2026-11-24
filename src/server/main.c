#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include "common.h"
#include "net.h"
#include "client_handler.h"
#include "exam.h"

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    int exams = exam_load_all("data/exams");
    if (exams < 0) {
        perror("[server] could not load data/exams");
        return 1;
    }
    printf("[server] loaded %d exam(s)\n", exams);

    int lfd = net_listen(SERVER_PORT);
    if (lfd < 0) { perror("listen"); return 1; }
    printf("[server] listening on port %d\n", SERVER_PORT);

    for (;;) {
        int cfd = net_accept(lfd);
        if (cfd < 0) continue;

        /* malloc so each thread owns its own copy of the fd (avoids a race on 'cfd') */
        int *arg = malloc(sizeof *arg);
        *arg = cfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
            perror("pthread_create");
            free(arg);
            continue;
        }
        pthread_detach(tid);   /* nobody will join it; resources freed on exit */
        printf("[server] client fd=%d connected\n", cfd);
    }
}
