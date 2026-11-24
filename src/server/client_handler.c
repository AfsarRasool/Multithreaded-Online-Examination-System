#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "common.h"
#include "net.h"
#include "protocol.h"
#include "client_handler.h"
#include "exam.h"
#include "exam_session.h"
#include "quiz.h"

typedef enum { ROLE_NONE, ROLE_ADMIN, ROLE_STUDENT, ROLE_PLAYER } Role;

/* Everything the server remembers about ONE connected client. Lives on the thread's stack. */
typedef struct {
    int  fd;
    Role role;
    char name[MAX_NAME];
    Exam *draft;
    Session *session;
    int pid;
    QuizRoom *quiz_room;
    int quiz_pid;
} ClientCtx;

static int require_admin(ClientCtx *client)
{
    if (client->role == ROLE_ADMIN) return 1;
    send_line(client->fd, "ERR|admin login required");
    return 0;
}

static void handle_exam(ClientCtx *client, Message *message)
{
    const char *command = message->cmd;
    if (!strcmp(command, "EXAM_CREATE")) {
        if (!require_admin(client)) return;
        if (message->argc != 2 || !message->args[0][0] ||
            atoi(message->args[1]) <= 0) {
            send_line(client->fd, "ERR|usage: EXAM_CREATE|subject|seconds");
            return;
        }

        Exam *draft = calloc(1, sizeof *draft);
        if (!draft) {
            send_line(client->fd, "ERR|could not create exam draft");
            return;
        }
        snprintf(draft->subject, sizeof draft->subject, "%s", message->args[0]);
        snprintf(draft->admin_name, sizeof draft->admin_name, "%s", client->name);
        draft->time_limit_sec = atoi(message->args[1]);
        free(client->draft);
        client->draft = draft;
        send_line(client->fd, "OK|draft started");

    } else if (!strcmp(command, "EXAM_ADDQ")) {
        if (!require_admin(client)) return;
        if (!client->draft) {
            send_line(client->fd, "ERR|EXAM_CREATE first");
            return;
        }
        char answer = message->argc == 7 && message->args[5][0]
            ? (char)toupper((unsigned char)message->args[5][0]) : 0;
        if (message->argc != 7 || answer < 'A' || answer > 'D' ||
            atoi(message->args[6]) <= 0 ||
            client->draft->num_questions >= MAX_QUESTIONS) {
            send_line(client->fd, "ERR|usage: EXAM_ADDQ|text|optA|optB|optC|optD|A-D|marks");
            return;
        }

        Question *question = &client->draft->questions[client->draft->num_questions++];
        snprintf(question->text, sizeof question->text, "%s", message->args[0]);
        for (int i = 0; i < 4; i++)
            snprintf(question->options[i], sizeof question->options[i], "%s", message->args[i + 1]);
        question->correct = answer;
        question->marks = atoi(message->args[6]);
        send_line(client->fd, "OK|question %d added", client->draft->num_questions);

    } else if (!strcmp(command, "EXAM_SAVE")) {
        if (!require_admin(client)) return;
        if (!client->draft || client->draft->num_questions == 0) {
            send_line(client->fd, "ERR|no questions in draft");
            return;
        }
        if (exam_register(client->draft) == 0) {
            send_line(client->fd, "OK|saved|%s", client->draft->id);
            client->draft = NULL;
        } else {
            send_line(client->fd, "ERR|could not save exam");
        }

    } else if (!strcmp(command, "EXAM_LIST")) {
        exam_list_send(client->fd);

    } else if (!strcmp(command, "EXAM_OPEN")) {
        if (!require_admin(client)) return;
        if (message->argc != 1) {
            send_line(client->fd, "ERR|usage: EXAM_OPEN|examId");
            return;
        }
        if (client->session) {
            send_line(client->fd, "ERR|session already open");
            return;
        }
        const Exam *exam = exam_find(message->args[0]);
        if (!exam) {
            send_line(client->fd, "ERR|exam not found");
            return;
        }
        client->session = session_create(exam, client->name, client->fd);
        if (!client->session) {
            send_line(client->fd, "ERR|could not create session");
            return;
        }
        send_line(client->fd, "OK|session|%s", client->session->code);

    } else if (!strcmp(command, "EXAM_START")) {
        if (!require_admin(client)) return;
        if (!client->session) {
            send_line(client->fd, "ERR|open a session first");
            return;
        }
        int result = session_start(client->session);
        if (result == -1) send_line(client->fd, "ERR|already started");
        else if (result == -2) send_line(client->fd, "ERR|no students joined yet");
        else if (result == -3) send_line(client->fd, "ERR|could not start timer");
        else send_line(client->fd, "OK|started|%d students", result);

    } else if (!strcmp(command, "EXAM_JOIN")) {
        if (client->role != ROLE_NONE || message->argc != 2 ||
            !message->args[1][0]) {
            send_line(client->fd, "ERR|usage: EXAM_JOIN|code|name");
            return;
        }
        Session *session = session_find(message->args[0]);
        if (!session) {
            send_line(client->fd, "ERR|no such session");
            return;
        }
        int pid = session_join(session, message->args[1], client->fd);
        if (pid == -1) send_line(client->fd, "ERR|exam already started");
        else if (pid == -2) send_line(client->fd, "ERR|session full");
        else if (pid == -3) send_line(client->fd, "ERR|name already taken");
        else {
            client->role = ROLE_STUDENT;
            client->session = session;
            client->pid = pid;
            snprintf(client->name, sizeof client->name, "%s", message->args[1]);
            send_line(client->fd, "OK|joined|%s|%s|%d", session->exam->subject,
                      session->admin_name, session->exam->time_limit_sec);
        }

    } else if (!strcmp(command, "ANSWER") || !strcmp(command, "SUBMIT")) {
        if (client->role != ROLE_STUDENT) {
            send_line(client->fd, "ERR|join an exam first");
            return;
        }
        if (!strcmp(command, "SUBMIT")) {
            int result = session_submit(client->session, client->pid);
            send_line(client->fd, result == 0 ? "OK|submitted" :
                      result == -1 ? "ERR|exam closed" : "ERR|invalid participant");
        } else {
            int result = -2;
            if (message->argc == 2 && message->args[1][0])
                result = session_answer(client->session, client->pid,
                                        atoi(message->args[0]),
                                        (char)toupper((unsigned char)message->args[1][0]));
            send_line(client->fd, result == 0 ? "OK|saved" :
                      result == -1 ? "ERR|exam closed" :
                      "ERR|invalid question or option");
        }
    }
}

static void handle_quiz(ClientCtx *client, Message *message)
{
    const char *command = message->cmd;
    if (!strcmp(command, "QUIZ_CREATE")) {
        if (!require_admin(client)) return;
        if (client->quiz_room) {
            send_line(client->fd, "ERR|you already created a quiz room");
            return;
        }
        if (message->argc != 2) {
            send_line(client->fd, "ERR|usage: QUIZ_CREATE|secondsPerQuestion|leaderboardEvery");
            return;
        }
        int seconds = atoi(message->args[0]);
        int leaderboard_every = atoi(message->args[1]);
        if (seconds < 1 || seconds > 3600 || leaderboard_every < 1 ||
            leaderboard_every > MAX_QUESTIONS) {
            send_line(client->fd, "ERR|time must be 1-3600 seconds and leaderboard interval 1-%d",
                      MAX_QUESTIONS);
            return;
        }
        client->quiz_room = quiz_room_create(client->name, client->fd, seconds,
                                             leaderboard_every);
        if (!client->quiz_room) {
            send_line(client->fd, "ERR|could not create quiz room");
            return;
        }
        send_line(client->fd, "OK|quiz_room|%s|%d|%d", client->quiz_room->code,
                  seconds, leaderboard_every);

    } else if (!strcmp(command, "QUIZ_ADDQ")) {
        if (!require_admin(client)) return;
        if (!client->quiz_room) {
            send_line(client->fd, "ERR|create a quiz room first");
            return;
        }
        if (message->argc != 6 || !message->args[5][0]) {
            send_line(client->fd, "ERR|usage: QUIZ_ADDQ|text|optA|optB|optC|optD|A-D");
            return;
        }
        char correct = (char)toupper((unsigned char)message->args[5][0]);
        if (strlen(message->args[5]) != 1 || correct < 'A' || correct > 'D') {
            send_line(client->fd, "ERR|correct option must be A, B, C, or D");
            return;
        }
        QuizQuestion question = {0};
        snprintf(question.text, sizeof question.text, "%s", message->args[0]);
        for (int i = 0; i < 4; i++)
            snprintf(question.options[i], sizeof question.options[i], "%s", message->args[i + 1]);
        question.correct = correct;
        int result = quiz_add_question(client->quiz_room, &question);
        if (result == 0)
            send_line(client->fd, "OK|quiz question added");
        else if (result == -2)
            send_line(client->fd, "ERR|quiz has reached the question limit");
        else
            send_line(client->fd, "ERR|quiz can no longer be edited");

    } else if (!strcmp(command, "QUIZ_START")) {
        if (!require_admin(client)) return;
        if (!client->quiz_room) {
            send_line(client->fd, "ERR|create a quiz room first");
            return;
        }
        int result = quiz_start(client->quiz_room);
        if (result == -1) send_line(client->fd, "ERR|quiz has already started or finished");
        else if (result == -2) send_line(client->fd, "ERR|add at least one question first");
        else if (result == -3) send_line(client->fd, "ERR|no players have joined");
        else if (result == -4) send_line(client->fd, "ERR|could not start quiz timer");
        else send_line(client->fd, "OK|quiz started|%d players", result);

    } else if (!strcmp(command, "QUIZ_JOIN")) {
        if (client->role != ROLE_NONE || message->argc != 2 ||
            !message->args[1][0]) {
            send_line(client->fd, "ERR|usage: QUIZ_JOIN|roomCode|name");
            return;
        }
        QuizRoom *room = quiz_room_find(message->args[0]);
        if (!room) {
            send_line(client->fd, "ERR|quiz room not found");
            return;
        }
        int player_id = quiz_join(room, message->args[1], client->fd);
        if (player_id == -1) send_line(client->fd, "ERR|quiz has already started");
        else if (player_id == -2) send_line(client->fd, "ERR|quiz room is full");
        else if (player_id == -3) send_line(client->fd, "ERR|name is already in this quiz");
        else {
            client->role = ROLE_PLAYER;
            client->quiz_room = room;
            client->quiz_pid = player_id;
            snprintf(client->name, sizeof client->name, "%s", message->args[1]);
            send_line(client->fd, "OK|quiz joined|%s|%d|%d", room->admin_name,
                      room->question_sec, room->leaderboard_every);
        }

    } else if (!strcmp(command, "QUIZ_ANSWER")) {
        if (client->role != ROLE_PLAYER) {
            send_line(client->fd, "ERR|join a quiz first");
            return;
        }
        if (message->argc != 1 || !message->args[0][0] ||
            strlen(message->args[0]) != 1) {
            send_line(client->fd, "ERR|answer with one letter: A, B, C, or D");
            return;
        }
        char answer = (char)toupper((unsigned char)message->args[0][0]);
        int result = quiz_answer(client->quiz_room, client->quiz_pid, answer);
        if (result == 0) send_line(client->fd, "OK|answer locked");
        else if (result == -1) send_line(client->fd, "ERR|question is closed");
        else if (result == -3) send_line(client->fd, "ERR|you already answered this question");
        else send_line(client->fd, "ERR|answer must be A, B, C, or D");
    }
}

static void handle_message(ClientCtx *c, Message *m)
{
    if (strcmp(m->cmd, "PING") == 0) {
        send_line(c->fd, "PONG");

    } else if (strcmp(m->cmd, "ADMIN_LOGIN") == 0) {         /* ADMIN_LOGIN|name|password */
        if (m->argc < 2)
            send_line(c->fd, "ERR|usage: ADMIN_LOGIN|name|password");
        else if (strcmp(m->args[1], ADMIN_PASSWORD) != 0)
            send_line(c->fd, "ERR|wrong password");
        else {
            c->role = ROLE_ADMIN;
            snprintf(c->name, MAX_NAME, "%s", m->args[0]);
            send_line(c->fd, "OK|welcome admin %s", c->name);
        }

    } else if (strcmp(m->cmd, "WHOAMI") == 0) {
        const char *r[] = {"none", "admin", "student", "player"};
        send_line(c->fd, "OK|%s|%s", r[c->role], c->name[0] ? c->name : "-");

    } else if (!strncmp(m->cmd, "EXAM_", 5) ||
               !strcmp(m->cmd, "ANSWER") || !strcmp(m->cmd, "SUBMIT")) {
        handle_exam(c, m);

    } else if (!strncmp(m->cmd, "QUIZ_", 5)) {
        handle_quiz(c, m);

    } else {
        send_line(c->fd, "ERR|unknown command %s", m->cmd);
    }
}

void *client_thread(void *arg)
{
    ClientCtx ctx = { .fd = *(int *)arg };
    free(arg);

    char line[MAX_LINE];
    send_line(ctx.fd, "OK|connected");

    while (recv_line(ctx.fd, line, sizeof line) > 0) {
        Message m;
        if (parse_message(line, &m) == 0 && m.cmd)
            handle_message(&ctx, &m);
    }
    if (ctx.role == ROLE_STUDENT) session_leave(ctx.session, ctx.pid);
    if (ctx.role == ROLE_PLAYER) quiz_leave(ctx.quiz_room, ctx.quiz_pid);
    if (ctx.role == ROLE_ADMIN && ctx.session) session_admin_gone(ctx.session);
    if (ctx.role == ROLE_ADMIN && ctx.quiz_room) quiz_admin_gone(ctx.quiz_room);
    free(ctx.draft);
    printf("[server] client fd=%d disconnected\n", ctx.fd);
    close(ctx.fd);
    return NULL;
}
