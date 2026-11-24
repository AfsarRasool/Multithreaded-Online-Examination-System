#ifndef EXAM_SESSION_H
#define EXAM_SESSION_H
#include <pthread.h>
#include <time.h>
#include "common.h"
#include "exam.h"

typedef enum { SESS_WAITING, SESS_RUNNING, SESS_FINISHED } SessionState;

typedef struct {
    char name[MAX_NAME];
    int  sockfd;
    char answers[MAX_QUESTIONS];  /* 0 = unanswered, else 'A'..'D' (latest wins) */
    int  score;
    int  submitted;               /* pressed SUBMIT */
    int  connected;
} Participant;

/* Session = one LIVE run of an Exam. Mutable, so it carries its own lock. */
typedef struct {
    char         code[CODE_LEN];
    const Exam  *exam;
    char         admin_name[MAX_NAME];
    int          admin_fd;
    SessionState state;
    Participant  participants[MAX_PARTICIPANTS];
    int          count;
    time_t       start_time;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} Session;

Session *session_create(const Exam *exam, const char *admin_name, int admin_fd);
Session *session_find(const char *code);
int session_join(Session *session, const char *name, int fd);
int session_start(Session *session);
int session_answer(Session *session, int pid, int question, char answer);
int session_submit(Session *session, int pid);
void session_leave(Session *session, int pid);
void session_admin_gone(Session *session);
int session_all_done(const Session *session);

void *exam_timer_thread(void *arg);
void exam_score_all(Session *session);
int exam_rank_order(const Session *session, int indices[]);
void exam_publish_results(Session *session);

#endif
