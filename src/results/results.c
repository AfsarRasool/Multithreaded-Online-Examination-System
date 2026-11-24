#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include "results.h"

static pthread_mutex_t results_lock = PTHREAD_MUTEX_INITIALIZER;

void results_save(const Session *session)
{
    int indices[MAX_PARTICIPANTS];
    int total = exam_total_marks(session->exam);
    int count = exam_rank_order(session, indices);
    time_t now = time(NULL);

    pthread_mutex_lock(&results_lock);
    FILE *file = fopen("data/results/results.txt", "a");
    if (!file) {
        perror("could not open data/results/results.txt");
        pthread_mutex_unlock(&results_lock);
        return;
    }

    fprintf(file, "=== Session %s | %s | Examiner: %s | %s", session->code,
            session->exam->subject, session->admin_name, ctime(&now));
    for (int rank = 0; rank < count; rank++) {
        const Participant *participant = &session->participants[indices[rank]];
        fprintf(file, "%d. %s  %d/%d\n", rank + 1, participant->name,
                participant->score, total);
    }
    if (fclose(file) != 0) perror("could not write data/results/results.txt");
    pthread_mutex_unlock(&results_lock);
}
