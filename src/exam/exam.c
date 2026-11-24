#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exam.h"
#include "net.h"
#include "protocol.h"

static Exam *exams[MAX_EXAMS];
static int exam_count;
static int next_id;
static pthread_mutex_t exam_lock = PTHREAD_MUTEX_INITIALIZER;

int exam_total_marks(const Exam *exam)
{
    int total = 0;
    for (int i = 0; i < exam->num_questions; i++)
        total += exam->questions[i].marks;
    return total;
}

static Exam *load_file(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file) return NULL;

    Exam *exam = calloc(1, sizeof *exam);
    if (!exam) {
        fclose(file);
        return NULL;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = '\0';
        Message message;
        if (parse_message(line, &message) != 0 || !message.cmd || message.argc < 1)
            continue;

        if (!strcmp(message.cmd, "ID")) {
            snprintf(exam->id, sizeof exam->id, "%s", message.args[0]);
        } else if (!strcmp(message.cmd, "SUBJECT")) {
            snprintf(exam->subject, sizeof exam->subject, "%s", message.args[0]);
        } else if (!strcmp(message.cmd, "ADMIN")) {
            snprintf(exam->admin_name, sizeof exam->admin_name, "%s", message.args[0]);
        } else if (!strcmp(message.cmd, "TIME")) {
            exam->time_limit_sec = atoi(message.args[0]);
        } else if (!strcmp(message.cmd, "Q") && message.argc == 7 &&
                   exam->num_questions < MAX_QUESTIONS) {
            Question *question = &exam->questions[exam->num_questions];
            char correct = message.args[5][0];
            int marks = atoi(message.args[6]);
            if (correct < 'A' || correct > 'D' || marks <= 0)
                continue;

            snprintf(question->text, sizeof question->text, "%s", message.args[0]);
            for (int i = 0; i < 4; i++)
                snprintf(question->options[i], sizeof question->options[i], "%s", message.args[i + 1]);
            question->correct = correct;
            question->marks = marks;
            exam->num_questions++;
        }
    }

    if (ferror(file)) {
        fclose(file);
        free(exam);
        return NULL;
    }
    fclose(file);
    if (!exam->id[0] || !exam->subject[0] || exam->time_limit_sec <= 0 ||
        exam->num_questions == 0) {
        free(exam);
        return NULL;
    }
    return exam;
}

int exam_load_all(const char *dir)
{
    DIR *directory = opendir(dir);
    if (!directory) return -1;

    int loaded = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".txt") != 0)
            continue;

        char path[512];
        int written = snprintf(path, sizeof path, "%s/%s", dir, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof path)
            continue;

        Exam *exam = load_file(path);
        if (!exam) continue;

        pthread_mutex_lock(&exam_lock);
        int duplicate = 0;
        for (int i = 0; i < exam_count; i++)
            if (!strcmp(exams[i]->id, exam->id)) duplicate = 1;
        if (exam_count < MAX_EXAMS && !duplicate) {
            exams[exam_count++] = exam;
            int id = exam->id[0] == 'E' ? atoi(exam->id + 1) : 0;
            if (id > next_id) next_id = id;
            loaded++;
            exam = NULL;
        }
        pthread_mutex_unlock(&exam_lock);
        free(exam);
    }

    closedir(directory);
    return loaded;
}

int exam_register(Exam *exam)
{
    if (!exam || !exam->subject[0] || !exam->admin_name[0] ||
        exam->time_limit_sec <= 0 || exam->num_questions <= 0 ||
        exam->num_questions > MAX_QUESTIONS)
        return -1;

    int result = -1;
    pthread_mutex_lock(&exam_lock);
    if (exam_count < MAX_EXAMS) {
        int id = next_id + 1;
        char path[128];
        snprintf(exam->id, sizeof exam->id, "E%d", id);
        int written = snprintf(path, sizeof path, "data/exams/%s.txt", exam->id);
        FILE *file = written >= 0 && (size_t)written < sizeof path ? fopen(path, "w") : NULL;
        if (file) {
            int failed = fprintf(file, "ID|%s\nSUBJECT|%s\nADMIN|%s\nTIME|%d\n",
                                exam->id, exam->subject, exam->admin_name,
                                exam->time_limit_sec) < 0;
            for (int i = 0; !failed && i < exam->num_questions; i++) {
                Question *question = &exam->questions[i];
                failed = fprintf(file, "Q|%s|%s|%s|%s|%s|%c|%d\n",
                                 question->text, question->options[0], question->options[1],
                                 question->options[2], question->options[3],
                                 question->correct, question->marks) < 0;
            }
            if (fclose(file) != 0) failed = 1;
            if (!failed) {
                exams[exam_count++] = exam;
                next_id = id;
                result = 0;
            } else {
                remove(path);
                exam->id[0] = '\0';
            }
        } else {
            exam->id[0] = '\0';
        }
    }
    pthread_mutex_unlock(&exam_lock);
    return result;
}

const Exam *exam_find(const char *id)
{
    if (!id) return NULL;
    const Exam *found = NULL;
    pthread_mutex_lock(&exam_lock);
    for (int i = 0; i < exam_count; i++) {
        if (!strcmp(exams[i]->id, id)) {
            found = exams[i];
            break;
        }
    }
    pthread_mutex_unlock(&exam_lock);
    return found;
}

void exam_list_send(int fd)
{
    pthread_mutex_lock(&exam_lock);
    for (int i = 0; i < exam_count; i++)
        send_line(fd, "EXAM|%s|%s|%s|%d|%d", exams[i]->id, exams[i]->subject,
                  exams[i]->admin_name, exams[i]->time_limit_sec, exams[i]->num_questions);
    pthread_mutex_unlock(&exam_lock);
    send_line(fd, "OK|end of list");
}
