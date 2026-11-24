#ifndef COMMON_H
#define COMMON_H

#define SERVER_PORT       5555
#define MAX_NAME          32
#define MAX_LINE          1024
#define MAX_ARGS          8
#define CODE_LEN          8
#define MAX_QUESTIONS     50
#define MAX_PARTICIPANTS  50
#define MAX_SESSIONS      16
#define MAX_ROOMS         16
#define MAX_EXAMS         64

/* Shared admin password (simple authentication, as decided) */
#define ADMIN_PASSWORD    "admin123"

/* Quiz defaults (admin can override when creating a room) */
#define DEFAULT_QUESTION_SEC      15
#define DEFAULT_LEADERBOARD_EVERY 1

#endif
