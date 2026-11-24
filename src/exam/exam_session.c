#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exam_session.h"
#include "net.h"

static Session *sessions[MAX_SESSIONS];
static int session_count;
static int next_code = 1000;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

Session *session_create(const Exam *exam, const char *admin_name, int admin_fd)
{
    if (!exam || !admin_name) return NULL;

    pthread_mutex_lock(&registry_lock);
    Session *session = NULL;
    if (session_count < MAX_SESSIONS) {
        session = calloc(1, sizeof *session);
        if (session) {
            session->exam = exam;
            session->state = SESS_WAITING;
            session->admin_fd = admin_fd;
            snprintf(session->admin_name, sizeof session->admin_name, "%s", admin_name);
            snprintf(session->code, sizeof session->code, "S%d", ++next_code);
            pthread_mutex_init(&session->lock, NULL);
            pthread_cond_init(&session->cond, NULL);
            sessions[session_count++] = session;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return session;
}

Session *session_find(const char *code)
{
    if (!code) return NULL;
    Session *found = NULL;
    pthread_mutex_lock(&registry_lock);
    for (int i = 0; i < session_count; i++) {
        if (!strcmp(sessions[i]->code, code)) {
            found = sessions[i];
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return found;
}

int session_all_done(const Session *session)
{
    if (session->count == 0) return 0;
    for (int i = 0; i < session->count; i++)
        if (session->participants[i].connected && !session->participants[i].submitted)
            return 0;
    return 1;
}

int session_join(Session *session, const char *name, int fd)
{
    if (!session || !name || !name[0]) return -1;

    pthread_mutex_lock(&session->lock);
    int result;
    if (session->state != SESS_WAITING) {
        result = -1;
    } else if (session->count >= MAX_PARTICIPANTS) {
        result = -2;
    } else {
        result = session->count;
        for (int i = 0; i < session->count; i++) {
            if (!strcmp(session->participants[i].name, name)) {
                result = -3;
                break;
            }
        }
        if (result >= 0) {
            Participant *participant = &session->participants[session->count++];
            snprintf(participant->name, sizeof participant->name, "%s", name);
            participant->sockfd = fd;
            participant->connected = 1;
            if (session->admin_fd >= 0)
                send_line(session->admin_fd, "EVT|JOINED|%s|%d", name, session->count);
        }
    }
    pthread_mutex_unlock(&session->lock);
    return result;
}

int session_start(Session *session)
{
    if (!session) return -1;
    pthread_mutex_lock(&session->lock);
    int result;
    if (session->state != SESS_WAITING) {
        result = -1;
    } else if (session->count == 0) {
        result = -2;
    } else {
        session->state = SESS_RUNNING;
        session->start_time = time(NULL);
        pthread_t timer;
        int error = pthread_create(&timer, NULL, exam_timer_thread, session);
        if (error != 0) {
            session->state = SESS_WAITING;
            result = -3;
        } else {
            pthread_detach(timer);
            const Exam *exam = session->exam;
            for (int i = 0; i < session->count; i++) {
                Participant *participant = &session->participants[i];
                if (!participant->connected) continue;
                send_line(participant->sockfd, "START|%s|%s|%d|%d", exam->subject,
                          session->admin_name, exam->time_limit_sec, exam->num_questions);
                for (int q = 0; q < exam->num_questions; q++) {
                    const Question *question = &exam->questions[q];
                    send_line(participant->sockfd, "Q|%d|%s|%s|%s|%s|%s|%d", q + 1,
                              question->text, question->options[0], question->options[1],
                              question->options[2], question->options[3], question->marks);
                }
            }
            result = session->count;
        }
    }
    pthread_mutex_unlock(&session->lock);
    return result;
}

int session_answer(Session *session, int pid, int question, char answer)
{
    if (!session) return -1;
    pthread_mutex_lock(&session->lock);
    int result = 0;
    if (session->state != SESS_RUNNING) {
        result = -1;
    } else if (pid < 0 || pid >= session->count || question < 1 ||
               question > session->exam->num_questions || answer < 'A' || answer > 'D') {
        result = -2;
    } else {
        session->participants[pid].answers[question - 1] = answer;
        session->participants[pid].submitted = 0;
    }
    pthread_mutex_unlock(&session->lock);
    return result;
}

int session_submit(Session *session, int pid)
{
    if (!session) return -1;
    pthread_mutex_lock(&session->lock);
    int result = 0;
    if (session->state != SESS_RUNNING) {
        result = -1;
    } else if (pid < 0 || pid >= session->count) {
        result = -2;
    } else {
        session->participants[pid].submitted = 1;
        if (session_all_done(session)) pthread_cond_signal(&session->cond);
    }
    pthread_mutex_unlock(&session->lock);
    return result;
}

void session_leave(Session *session, int pid)
{
    if (!session) return;
    pthread_mutex_lock(&session->lock);
    if (pid >= 0 && pid < session->count) {
        session->participants[pid].connected = 0;
        session->participants[pid].sockfd = -1;
        if (session->state == SESS_RUNNING && session_all_done(session))
            pthread_cond_signal(&session->cond);
    }
    pthread_mutex_unlock(&session->lock);
}

void session_admin_gone(Session *session)
{
    if (!session) return;
    pthread_mutex_lock(&session->lock);
    session->admin_fd = -1;
    pthread_mutex_unlock(&session->lock);
}
