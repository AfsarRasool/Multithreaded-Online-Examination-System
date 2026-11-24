#ifndef QUIZ_H
#define QUIZ_H
#include <pthread.h>
#include <time.h>
#include "common.h"

typedef struct {
    char text[256];
    char options[4][128];
    char correct;
} QuizQuestion;

typedef struct {
    char name[MAX_NAME];
    int  sockfd;
    int  score;
    int  answered_current;
    int  points_current;
    int  rank_current;
} QuizPlayer;

typedef enum {
    QUIZ_WAITING,
    QUIZ_RUNNING,
    QUIZ_FINISHED
} QuizState;

typedef struct {
    char code[CODE_LEN];
    char admin_name[MAX_NAME];
    int  admin_fd;
    int  question_sec;            /* default 15 */
    int  leaderboard_every;       /* default 1  */
    int  num_questions;
    QuizQuestion questions[MAX_QUESTIONS];
    int  current_q;               /* zero-based, -1 before the first question */
    int  answer_rank_counter;
    QuizPlayer players[MAX_PARTICIPANTS];
    int  player_count;
    QuizState state;
    struct timespec question_deadline;
    pthread_mutex_t lock;
} QuizRoom;

QuizRoom *quiz_room_create(const char *admin_name, int admin_fd,
                           int question_sec, int leaderboard_every);
QuizRoom *quiz_room_find(const char *code);
int quiz_add_question(QuizRoom *room, const QuizQuestion *question);
int quiz_join(QuizRoom *room, const char *name, int fd);
int quiz_start(QuizRoom *room);
int quiz_answer(QuizRoom *room, int player_id, char answer);
void quiz_leave(QuizRoom *room, int player_id);
void quiz_admin_gone(QuizRoom *room);

#endif
