#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "quiz.h"
#include "net.h"

static QuizRoom *rooms[MAX_ROOMS];
static int room_count;
static int next_room_code = 1000;
static pthread_mutex_t rooms_lock = PTHREAD_MUTEX_INITIALIZER;

static void send_room(QuizRoom *room, const char *format, ...)
{
    char line[MAX_LINE];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);

    if (room->admin_fd >= 0) send_line(room->admin_fd, "%s", line);
    for (int i = 0; i < room->player_count; i++)
        if (room->players[i].sockfd >= 0)
            send_line(room->players[i].sockfd, "%s", line);
}

QuizRoom *quiz_room_create(const char *admin_name, int admin_fd,
                           int question_sec, int leaderboard_every)
{
    if (!admin_name || !admin_name[0] || admin_fd < 0 ||
        question_sec <= 0 || leaderboard_every <= 0)
        return NULL;

    pthread_mutex_lock(&rooms_lock);
    QuizRoom *room = NULL;
    if (room_count < MAX_ROOMS) {
        room = calloc(1, sizeof *room);
        if (room) {
            snprintf(room->code, sizeof room->code, "Q%d", ++next_room_code);
            snprintf(room->admin_name, sizeof room->admin_name, "%s", admin_name);
            room->admin_fd = admin_fd;
            room->question_sec = question_sec;
            room->leaderboard_every = leaderboard_every;
            room->current_q = -1;
            room->state = QUIZ_WAITING;
            pthread_mutex_init(&room->lock, NULL);
            rooms[room_count++] = room;
        }
    }
    pthread_mutex_unlock(&rooms_lock);
    return room;
}

QuizRoom *quiz_room_find(const char *code)
{
    if (!code) return NULL;
    QuizRoom *found = NULL;
    pthread_mutex_lock(&rooms_lock);
    for (int i = 0; i < room_count; i++) {
        if (!strcmp(rooms[i]->code, code)) {
            found = rooms[i];
            break;
        }
    }
    pthread_mutex_unlock(&rooms_lock);
    return found;
}

int quiz_add_question(QuizRoom *room, const QuizQuestion *question)
{
    if (!room || !question || !question->text[0] ||
        question->correct < 'A' || question->correct > 'D')
        return -1;
    for (int i = 0; i < 4; i++)
        if (!question->options[i][0]) return -1;

    pthread_mutex_lock(&room->lock);
    int result = 0;
    if (room->state != QUIZ_WAITING) {
        result = -1;
    } else if (room->num_questions >= MAX_QUESTIONS) {
        result = -2;
    } else {
        room->questions[room->num_questions++] = *question;
    }
    pthread_mutex_unlock(&room->lock);
    return result;
}

int quiz_join(QuizRoom *room, const char *name, int fd)
{
    if (!room || !name || !name[0] || fd < 0) return -1;

    pthread_mutex_lock(&room->lock);
    int result;
    if (room->state != QUIZ_WAITING) {
        result = -1;
    } else if (room->player_count >= MAX_PARTICIPANTS) {
        result = -2;
    } else {
        result = room->player_count;
        for (int i = 0; i < room->player_count; i++) {
            if (!strcmp(room->players[i].name, name)) {
                result = -3;
                break;
            }
        }
        if (result >= 0) {
            QuizPlayer *player = &room->players[room->player_count++];
            snprintf(player->name, sizeof player->name, "%s", name);
            player->sockfd = fd;
            if (room->admin_fd >= 0)
                send_line(room->admin_fd, "QUIZ_EVT|JOINED|%s|%d",
                          player->name, room->player_count);
        }
    }
    pthread_mutex_unlock(&room->lock);
    return result;
}

static void wait_for_question_end(int seconds)
{
    struct timespec remaining = { .tv_sec = seconds, .tv_nsec = 0 };
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {}
}

static void publish_leaderboard(QuizRoom *room)
{
    int order[MAX_PARTICIPANTS];
    for (int i = 0; i < room->player_count; i++) order[i] = i;
    for (int i = 0; i < room->player_count; i++) {
        for (int j = 0; j + 1 < room->player_count - i; j++) {
            if (room->players[order[j]].score < room->players[order[j + 1]].score) {
                int temp = order[j];
                order[j] = order[j + 1];
                order[j + 1] = temp;
            }
        }
    }

    for (int rank = 0; rank < room->player_count; rank++) {
        QuizPlayer *player = &room->players[order[rank]];
        send_room(room, "QUIZ_BOARD|%d|%s|%d", rank + 1, player->name, player->score);
    }
    send_room(room, "QUIZ_BOARD_END");
}

static void *quiz_timer_thread(void *arg)
{
    QuizRoom *room = arg;

    for (int question_index = 0; question_index < room->num_questions; question_index++) {
        pthread_mutex_lock(&room->lock);
        if (room->state != QUIZ_RUNNING) {
            pthread_mutex_unlock(&room->lock);
            return NULL;
        }

        room->current_q = question_index;
        room->answer_rank_counter = 0;
        for (int i = 0; i < room->player_count; i++) {
            room->players[i].answered_current = 0;
            room->players[i].points_current = 0;
            room->players[i].rank_current = 0;
        }

        QuizQuestion *question = &room->questions[question_index];
        if (question_index == 0)
            send_room(room, "QUIZ_BEGIN|%d|%d|%d", room->question_sec,
                      room->leaderboard_every, room->num_questions);
        send_room(room, "QUIZ_QUESTION|%d|%d|%s|%s|%s|%s|%s|%d",
                  question_index + 1, room->num_questions, question->text,
                  question->options[0], question->options[1], question->options[2],
                  question->options[3], room->question_sec);

        if (clock_gettime(CLOCK_MONOTONIC, &room->question_deadline) != 0) {
            perror("clock_gettime");
            room->state = QUIZ_FINISHED;
            pthread_mutex_unlock(&room->lock);
            return NULL;
        }
        room->question_deadline.tv_sec += room->question_sec;
        pthread_mutex_unlock(&room->lock);

        wait_for_question_end(room->question_sec);

        pthread_mutex_lock(&room->lock);
        question = &room->questions[question_index];
        send_room(room, "QUIZ_REVEAL|%d|%c|%s", question_index + 1,
                  question->correct, question->options[question->correct - 'A']);
        for (int i = 0; i < room->player_count; i++) {
            QuizPlayer *player = &room->players[i];
            if (player->sockfd >= 0)
                send_line(player->sockfd, "QUIZ_YOU|%d|%d|%d",
                          player->points_current, player->score, player->rank_current);
        }

        int is_last = question_index + 1 == room->num_questions;
        if ((question_index + 1) % room->leaderboard_every == 0 || is_last)
            publish_leaderboard(room);

        if (is_last) {
            room->state = QUIZ_FINISHED;
            room->current_q = room->num_questions;
            send_room(room, "QUIZ_END");
        }
        pthread_mutex_unlock(&room->lock);
    }
    return NULL;
}

int quiz_start(QuizRoom *room)
{
    if (!room) return -1;
    pthread_mutex_lock(&room->lock);
    int result;
    if (room->state != QUIZ_WAITING) {
        result = -1;
    } else if (room->num_questions == 0) {
        result = -2;
    } else if (room->player_count == 0) {
        result = -3;
    } else {
        room->state = QUIZ_RUNNING;
        pthread_t timer;
        int error = pthread_create(&timer, NULL, quiz_timer_thread, room);
        if (error != 0) {
            room->state = QUIZ_WAITING;
            result = -4;
        } else {
            pthread_detach(timer);
            result = room->player_count;
        }
    }
    pthread_mutex_unlock(&room->lock);
    return result;
}

int quiz_answer(QuizRoom *room, int player_id, char answer)
{
    if (!room) return -1;
    pthread_mutex_lock(&room->lock);
    int result = 0;
    if (room->state != QUIZ_RUNNING || room->current_q < 0 ||
        room->current_q >= room->num_questions) {
        result = -1;
    } else if (player_id < 0 || player_id >= room->player_count ||
               answer < 'A' || answer > 'D') {
        result = -2;
    } else {
        QuizPlayer *player = &room->players[player_id];
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            result = -1;
        } else if (now.tv_sec > room->question_deadline.tv_sec ||
                   (now.tv_sec == room->question_deadline.tv_sec &&
                    now.tv_nsec >= room->question_deadline.tv_nsec)) {
            result = -1;
        } else if (player->answered_current) {
            result = -3;
        } else {
            player->answered_current = 1;
            QuizQuestion *question = &room->questions[room->current_q];
            if (answer == question->correct) {
                long remaining_ns =
                    (room->question_deadline.tv_sec - now.tv_sec) * 1000000000L +
                    room->question_deadline.tv_nsec - now.tv_nsec;
                if (remaining_ns <= 5000000000L) {
                    player->points_current = 50;
                } else {
                    int rank = ++room->answer_rank_counter;
                    int points = 100 - 10 * (rank - 1);
                    player->points_current = points < 50 ? 50 : points;
                    player->rank_current = rank;
                }
                player->score += player->points_current;
            }
        }
    }
    pthread_mutex_unlock(&room->lock);
    return result;
}

void quiz_leave(QuizRoom *room, int player_id)
{
    if (!room) return;
    pthread_mutex_lock(&room->lock);
    if (player_id >= 0 && player_id < room->player_count)
        room->players[player_id].sockfd = -1;
    pthread_mutex_unlock(&room->lock);
}

void quiz_admin_gone(QuizRoom *room)
{
    if (!room) return;
    pthread_mutex_lock(&room->lock);
    room->admin_fd = -1;
    pthread_mutex_unlock(&room->lock);
}
