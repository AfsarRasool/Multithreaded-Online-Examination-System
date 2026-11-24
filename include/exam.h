#ifndef EXAM_H
#define EXAM_H
#include "common.h"

typedef struct {
    char text[256];
    char options[4][128];   /* A, B, C, D */
    char correct;           /* 'A'..'D' */
    int  marks;
} Question;

/* Exam = read-only template. Once created it never changes, so it needs no lock. */
typedef struct {
    char     id[CODE_LEN];
    char     subject[64];
    char     admin_name[MAX_NAME];   /* who created it: shown to students */
    int      time_limit_sec;
    int      num_questions;
    Question questions[MAX_QUESTIONS];
} Exam;

int exam_load_all(const char *dir);
int exam_register(Exam *exam);
const Exam *exam_find(const char *id);
void exam_list_send(int fd);
int exam_total_marks(const Exam *exam);

#endif
