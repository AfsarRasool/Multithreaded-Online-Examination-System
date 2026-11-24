#include <errno.h>
#include "exam_session.h"
#include "results.h"

void *exam_timer_thread(void *arg)
{
    Session *session = arg;
    pthread_mutex_lock(&session->lock);

    struct timespec deadline = {
        .tv_sec = session->start_time + session->exam->time_limit_sec,
        .tv_nsec = 0
    };
    while (session->state == SESS_RUNNING && !session_all_done(session)) {
        int result = pthread_cond_timedwait(&session->cond, &session->lock, &deadline);
        if (result == ETIMEDOUT) break;
        if (result != 0) {
            session->state = SESS_FINISHED;
            pthread_mutex_unlock(&session->lock);
            return NULL;
        }
    }

    if (session->state == SESS_RUNNING) {
        session->state = SESS_FINISHED;
        exam_score_all(session);
        exam_publish_results(session);
        results_save(session);
    }
    pthread_mutex_unlock(&session->lock);
    return NULL;
}
