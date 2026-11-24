#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#include "common.h"
#include "net.h"
#include "protocol.h"

static int socket_fd;
static int connection_alive = 1;
static int connection_ready;
static unsigned long response_number;
static char last_response[MAX_LINE];
static pthread_mutex_t response_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t response_ready = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t ui_lock = PTHREAD_MUTEX_INITIALIZER;
static int interactive_ui;
static int terminal_rows = 24;
static int terminal_columns = 80;
static int panel_column = 49;
static int left_width = 46;
static int panel_text_width = 28;
static int cursor_row = 1;
static int cursor_column = 1;
static char server_log[128][MAX_LINE];
static int server_log_count;
static volatile sig_atomic_t interrupted;
static int wake_pipe[2] = {-1, -1};
static int completion_pending;
static int navigation_action;

static void render_server_panel(void);
static int read_line_internal(char line[MAX_LINE], int watch_completion);

enum {
    NAVIGATION_NONE,
    NAVIGATION_HOME,
    NAVIGATION_EXIT,
    READ_COMPLETED = -2
};

static void handle_interrupt(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static void initialize_ui(void)
{
    struct winsize size;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) ||
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 ||
        size.ws_col < 72 || size.ws_row < 10)
        return;

    interactive_ui = 1;
    terminal_rows = size.ws_row;
    terminal_columns = size.ws_col;
    panel_column = terminal_columns * 58 / 100;
    if (panel_column < 42) panel_column = 42;
    left_width = panel_column - 3;
    panel_text_width = terminal_columns - panel_column - 3;

    fputs("\033[2J\033[H", stdout);
    render_server_panel();
}

static void render_server_panel(void)
{
    if (!interactive_ui) return;

    int rows = terminal_rows - 2;
    if (rows > 24) rows = 24;
    if (rows < 3) rows = 3;
    int first_log = server_log_count - (rows - 2);
    if (first_log < 0) first_log = 0;

    fputs("\033[s", stdout);
    for (int row = 1; row <= rows; row++) {
        printf("\033[%d;%dH\033[K", row, panel_column);
        if (row == 1) {
            printf("| SERVER MESSAGES");
        } else if (row == 2) {
            printf("|%.*s", terminal_columns - panel_column - 1,
                   "-----------------------------------------------");
        } else {
            int log_index = first_log + row - 3;
            if (log_index < server_log_count)
                printf("| %-*.*s", panel_text_width, panel_text_width,
                       server_log[log_index]);
            else
                printf("|");
        }
    }
    fputs("\033[u", stdout);
    fflush(stdout);
}

static void append_server_log_line(const char *line)
{
    if (server_log_count == (int)(sizeof server_log / sizeof server_log[0])) {
        memmove(server_log, server_log + 1,
                (sizeof server_log / sizeof server_log[0] - 1) * sizeof server_log[0]);
        server_log_count--;
    }
    snprintf(server_log[server_log_count], sizeof server_log[0], "%s", line);
    server_log_count++;
}

static void add_server_message(const char *message)
{
    if (!interactive_ui) {
        fprintf(stdout, "\n[server] %s\n", message);
        fflush(stdout);
        return;
    }

    pthread_mutex_lock(&ui_lock);
    const char *part = message;
    while (*part) {
        while (*part == ' ') part++;
        if (!*part) break;

        size_t length = strlen(part);
        if (length > (size_t)panel_text_width) {
            length = (size_t)panel_text_width;
            const char *space = NULL;
            for (size_t i = 0; i < length; i++)
                if (part[i] == ' ') space = part + i;
            if (space && space != part) length = (size_t)(space - part);
        }

        char wrapped[MAX_LINE];
        snprintf(wrapped, sizeof wrapped, "%.*s", (int)length, part);
        append_server_log_line(wrapped);
        part += length;
    }
    render_server_panel();
    pthread_mutex_unlock(&ui_lock);
}

static void add_server_section(const char *title)
{
    char heading[MAX_LINE];
    snprintf(heading, sizeof heading, "=== %s ===", title);
    if (!interactive_ui) {
        fprintf(stdout, "\n%s\n", heading);
        fflush(stdout);
        return;
    }

    pthread_mutex_lock(&ui_lock);
    append_server_log_line("");
    append_server_log_line(heading);
    render_server_panel();
    pthread_mutex_unlock(&ui_lock);
}

static void ui_printf(const char *format, ...)
{
    char text[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);

    if (!interactive_ui) {
        fputs(text, stdout);
        fflush(stdout);
        return;
    }

    pthread_mutex_lock(&ui_lock);
    for (const char *p = text; *p; p++) {
        if (*p == '\n' || cursor_column > left_width) {
            fputc('\n', stdout);
            cursor_column = 1;
            if (cursor_row < terminal_rows) cursor_row++;
            if (*p == '\n') continue;
        }
        fputc(*p, stdout);
        cursor_column++;
    }
    render_server_panel();
    fflush(stdout);
    pthread_mutex_unlock(&ui_lock);
}

#define printf ui_printf

static void display_server_line(const char *line)
{
    char copy[MAX_LINE];
    snprintf(copy, sizeof copy, "%s", line);

    Message message;
    if (parse_message(copy, &message) != 0 || !message.cmd) {
        add_server_message(line);
        return;
    }

    if (!strcmp(message.cmd, "START") && message.argc >= 4) {
        char text[MAX_LINE];
        snprintf(text, sizeof text, "Exam started: %s", message.args[0]);
        add_server_message(text);
        snprintf(text, sizeof text, "Teacher: %s", message.args[1]);
        add_server_message(text);
        snprintf(text, sizeof text, "Time: %s seconds", message.args[2]);
        add_server_message(text);
        snprintf(text, sizeof text, "Questions: %s", message.args[3]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "QUIZ_BEGIN") && message.argc >= 3) {
        char text[MAX_LINE];
        snprintf(text, sizeof text, "Quiz is live: %s sec per question",
                 message.args[0]);
        add_server_message(text);
        snprintf(text, sizeof text, "Leaderboard every %s question(s)", message.args[1]);
        add_server_message(text);
        snprintf(text, sizeof text, "%s question(s) total", message.args[2]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "QUIZ_QUESTION") && message.argc >= 8) {
        char text[MAX_LINE];
        add_server_section("QUIZ QUESTION");
        snprintf(text, sizeof text, "Quiz question %s of %s: %s",
                 message.args[0], message.args[1], message.args[2]);
        add_server_message(text);
        for (int i = 0; i < 4; i++) {
            snprintf(text, sizeof text, "%c) %s", 'A' + i, message.args[i + 3]);
            add_server_message(text);
        }
        snprintf(text, sizeof text, "Answer within %s seconds", message.args[7]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "QUIZ_REVEAL") && message.argc >= 3) {
        char text[MAX_LINE];
        add_server_section("ANSWER REVEAL");
        snprintf(text, sizeof text, "Answer to question %s: %s) %s",
                 message.args[0], message.args[1], message.args[2]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "QUIZ_YOU") && message.argc >= 3) {
        char text[MAX_LINE];
        int points = atoi(message.args[0]);
        int rank = atoi(message.args[2]);
        add_server_section("YOUR QUESTION SCORE");
        if (rank > 0)
            snprintf(text, sizeof text, "You earned %d points (correct answer rank %d)",
                     points, rank);
        else if (points > 0)
            snprintf(text, sizeof text, "You earned %d fixed points in the final five seconds",
                     points);
        else
            snprintf(text, sizeof text, "No points for this question");
        add_server_message(text);
        snprintf(text, sizeof text, "Your total score: %s", message.args[1]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "QUIZ_BOARD") && message.argc >= 3) {
        char text[MAX_LINE];
        if (!strcmp(message.args[0], "1"))
            add_server_section("QUIZ LEADERBOARD");
        snprintf(text, sizeof text, "%s. %s: %s points", message.args[0],
                 message.args[1], message.args[2]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "QUIZ_BOARD_END")) {
        add_server_message("End of quiz leaderboard");
    } else if (!strcmp(message.cmd, "QUIZ_END")) {
        add_server_section("QUIZ COMPLETE");
        add_server_message("Quiz finished");
        pthread_mutex_lock(&response_lock);
        completion_pending = 1;
        pthread_cond_broadcast(&response_ready);
        pthread_mutex_unlock(&response_lock);
        if (wake_pipe[1] >= 0) {
            char byte = 1;
            write(wake_pipe[1], &byte, sizeof byte);
        }
    } else if (!strcmp(message.cmd, "QUIZ_EVT") && message.argc >= 3 &&
               !strcmp(message.args[0], "JOINED")) {
        char text[MAX_LINE];
        snprintf(text, sizeof text, "%s joined the quiz (%s player(s))",
                 message.args[1], message.args[2]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "Q") && message.argc >= 7) {
        char text[MAX_LINE];
        snprintf(text, sizeof text, "Question %s: %s", message.args[0], message.args[1]);
        add_server_message(text);
        for (int i = 0; i < 4; i++) {
            snprintf(text, sizeof text, "%c) %s", 'A' + i, message.args[i + 2]);
            add_server_message(text);
        }
    } else if (!strcmp(message.cmd, "RESULT") && message.argc >= 3) {
        char text[MAX_LINE];
        add_server_section("EXAM RESULT");
        snprintf(text, sizeof text, "Your result: %s / %s (rank %s)",
                 message.args[0], message.args[1], message.args[2]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "BOARD") && message.argc >= 4) {
        char text[MAX_LINE];
        if (!strcmp(message.args[0], "1"))
            add_server_section("EXAM LEADERBOARD");
        snprintf(text, sizeof text, "%s. %s: %s / %s", message.args[0],
                 message.args[1], message.args[2], message.args[3]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "BOARD_END")) {
        add_server_section("EXAM COMPLETE");
        add_server_message("End of results");
        pthread_mutex_lock(&response_lock);
        completion_pending = 1;
        pthread_cond_broadcast(&response_ready);
        pthread_mutex_unlock(&response_lock);
        if (wake_pipe[1] >= 0) {
            char byte = 1;
            write(wake_pipe[1], &byte, sizeof byte);
        }
    } else if (!strcmp(message.cmd, "EVT") && message.argc >= 3 &&
               !strcmp(message.args[0], "JOINED")) {
        char text[MAX_LINE];
        snprintf(text, sizeof text, "%s joined (%s students)", message.args[1],
                 message.args[2]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "EXAM") && message.argc >= 5) {
        char text[MAX_LINE];
        snprintf(text, sizeof text, "%s: %s, %s sec, %s questions",
                 message.args[0], message.args[1], message.args[3], message.args[4]);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "OK") && message.argc >= 1 &&
               !strcmp(message.args[0], "end of list")) {
        add_server_message("End of exam list");
    } else if (!strcmp(message.cmd, "ERR")) {
        char text[MAX_LINE];
        snprintf(text, sizeof text, "Error: %s", line + 4);
        add_server_message(text);
    } else if (!strcmp(message.cmd, "OK")) {
        add_server_message(line + 3);
    } else {
        add_server_message(line);
    }
}

static int is_response(const char *line)
{
    return !strncmp(line, "OK|", 3) || !strncmp(line, "ERR|", 4);
}

static void *server_reader(void *unused)
{
    (void)unused;
    char line[MAX_LINE];
    while (recv_line(socket_fd, line, sizeof line) > 0) {
        display_server_line(line);
        if (is_response(line)) {
            pthread_mutex_lock(&response_lock);
            snprintf(last_response, sizeof last_response, "%s", line);
            response_number++;
            if (!strcmp(line, "OK|connected")) connection_ready = 1;
            pthread_cond_broadcast(&response_ready);
            pthread_mutex_unlock(&response_lock);
        }
    }

    add_server_message("Connection to server closed");
    pthread_mutex_lock(&response_lock);
    connection_alive = 0;
    pthread_cond_broadcast(&response_ready);
    pthread_mutex_unlock(&response_lock);
    return NULL;
}

static int send_and_wait(const char *command, char response[MAX_LINE])
{
    pthread_mutex_lock(&response_lock);
    unsigned long previous_response = response_number;
    int alive = connection_alive;
    pthread_mutex_unlock(&response_lock);

    if (!alive || send_line(socket_fd, "%s", command) < 0) {
        snprintf(response, MAX_LINE, "ERR|connection lost");
        return 0;
    }

    pthread_mutex_lock(&response_lock);
    while (response_number == previous_response && connection_alive)
        pthread_cond_wait(&response_ready, &response_lock);
    if (response_number != previous_response)
        snprintf(response, MAX_LINE, "%s", last_response);
    else
        snprintf(response, MAX_LINE, "ERR|connection lost");
    pthread_mutex_unlock(&response_lock);
    return strncmp(response, "OK|", 3) == 0;
}

static int read_line_internal(char line[MAX_LINE], int watch_completion)
{
    if (interactive_ui) {
        struct termios original, input;
        struct sigaction action, previous_action;
        if (tcgetattr(STDIN_FILENO, &original) != 0) return 0;
        input = original;
        input.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        input.c_cc[VMIN] = 1;
        input.c_cc[VTIME] = 0;
        memset(&action, 0, sizeof action);
        action.sa_handler = handle_interrupt;
        sigemptyset(&action.sa_mask);
        interrupted = 0;
        sigaction(SIGINT, &action, &previous_action);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &input) != 0) {
            sigaction(SIGINT, &previous_action, NULL);
            return 0;
        }

        int input_rows[MAX_LINE];
        int input_columns[MAX_LINE];
        size_t length = 0;
        int overflow = 0;
        int completion_after_line = 0;
        for (;;) {
            struct pollfd fds[2];
            fds[0].fd = STDIN_FILENO;
            fds[0].events = POLLIN;
            fds[0].revents = 0;
            nfds_t fd_count = 1;
            if (watch_completion && wake_pipe[0] >= 0) {
                fds[1].fd = wake_pipe[0];
                fds[1].events = POLLIN;
                fds[1].revents = 0;
                fd_count = 2;
            }
            int ready = poll(fds, fd_count, -1);
            if (ready < 0 && errno == EINTR && interrupted) break;
            if (ready < 0) continue;
            if (fd_count == 2 && (fds[1].revents & POLLIN)) {
                char byte;
                read(wake_pipe[0], &byte, sizeof byte);
                pthread_mutex_lock(&response_lock);
                int completed = completion_pending;
                pthread_mutex_unlock(&response_lock);
                if (completed) {
                    if (length == 0) {
                        tcsetattr(STDIN_FILENO, TCSANOW, &original);
                        sigaction(SIGINT, &previous_action, NULL);
                        return READ_COMPLETED;
                    }
                    completion_after_line = 1;
                }
            }
            if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;

            unsigned char character;
            ssize_t count = read(STDIN_FILENO, &character, 1);
            if (count < 0 && errno == EINTR && interrupted) break;
            if (count <= 0) {
                tcsetattr(STDIN_FILENO, TCSANOW, &original);
                sigaction(SIGINT, &previous_action, NULL);
                return 0;
            }
            if (character == '\r' || character == '\n') {
                pthread_mutex_lock(&ui_lock);
                fputc('\n', stdout);
                cursor_column = 1;
                if (cursor_row < terminal_rows) cursor_row++;
                render_server_panel();
                pthread_mutex_unlock(&ui_lock);
                line[length] = '\0';
                tcsetattr(STDIN_FILENO, TCSANOW, &original);
                sigaction(SIGINT, &previous_action, NULL);
                if (completion_after_line) return READ_COMPLETED;
                return overflow ? -1 : 1;
            }
            if (character == 4 && length == 0) {
                tcsetattr(STDIN_FILENO, TCSANOW, &original);
                sigaction(SIGINT, &previous_action, NULL);
                return 0;
            }
            if (character == 8 || character == 127) {
                if (length > 0) {
                    length--;
                    pthread_mutex_lock(&ui_lock);
                    cursor_row = input_rows[length];
                    cursor_column = input_columns[length];
                    fprintf(stdout, "\033[%d;%dH \033[%d;%dH",
                            cursor_row, cursor_column, cursor_row, cursor_column);
                    render_server_panel();
                    pthread_mutex_unlock(&ui_lock);
                }
                continue;
            }
            if (!isprint(character)) continue;
            if (length >= sizeof input_rows / sizeof input_rows[0] - 1) {
                overflow = 1;
                continue;
            }

            pthread_mutex_lock(&ui_lock);
            if (cursor_column > left_width) {
                fputc('\n', stdout);
                cursor_column = 1;
                if (cursor_row < terminal_rows) cursor_row++;
            }
            input_rows[length] = cursor_row;
            input_columns[length] = cursor_column;
            line[length++] = (char)character;
            fputc(character, stdout);
            cursor_column++;
            render_server_panel();
            pthread_mutex_unlock(&ui_lock);
        }

        tcsetattr(STDIN_FILENO, TCSANOW, &original);
        sigaction(SIGINT, &previous_action, NULL);
        return 0;
    }

    if (!fgets(line, MAX_LINE, stdin)) return 0;
    size_t length = strlen(line);
    if (length && line[length - 1] == '\n') {
        line[--length] = '\0';
        if (length && line[length - 1] == '\r') line[--length] = '\0';
        return 1;
    }
    if (!feof(stdin)) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF) {}
        return -1;
    }
    return 1;
}

static int read_line(char line[MAX_LINE])
{
    return read_line_internal(line, 1);
}

static void completion_menu(void)
{
    char line[MAX_LINE];
    printf("\n=== SESSION COMPLETE ===\n"
           "1. Return to home menu\n"
           "2. Exit the program\n");
    for (;;) {
        char *end;
        printf("Choose an option: ");
        fflush(stdout);
        int result = read_line_internal(line, 0);
        if (result <= 0) {
            navigation_action = NAVIGATION_EXIT;
            break;
        }
        long choice = strtol(line, &end, 10);
        while (*end == ' ' || *end == '\t') end++;
        if (end != line && *end == '\0' && (choice == 1 || choice == 2)) {
            navigation_action = choice == 1 ? NAVIGATION_HOME : NAVIGATION_EXIT;
            break;
        }
        printf("Enter 1 to return home or 2 to exit.\n");
    }

    pthread_mutex_lock(&response_lock);
    completion_pending = 0;
    pthread_mutex_unlock(&response_lock);
}
static int ask_text(const char *prompt, char *value, size_t size)
{
    char line[MAX_LINE];
    for (;;) {
        printf("%s", prompt);
        fflush(stdout);
        int result = read_line(line);
        if (result == READ_COMPLETED) {
            completion_menu();
            return 0;
        }
        if (result == 0) return 0;
        if (result < 0 || strlen(line) >= size) {
            printf("That entry is too long. Please try again.\n");
            continue;
        }
        if (!line[0]) {
            printf("Please enter a value.\n");
            continue;
        }
        if (strchr(line, '|')) {
            printf("Please do not use the | character.\n");
            continue;
        }
        snprintf(value, size, "%s", line);
        return 1;
    }
}

static int ask_number(const char *prompt, int minimum, int maximum, int *value)
{
    char line[MAX_LINE];
    for (;;) {
        char *end;
        printf("%s", prompt);
        fflush(stdout);
        int result = read_line(line);
        if (result == READ_COMPLETED) {
            completion_menu();
            return 0;
        }
        if (result == 0) return 0;
        long number = strtol(line, &end, 10);
        while (*end == ' ' || *end == '\t') end++;
        if (result > 0 && end != line && *end == '\0' &&
            number >= minimum && number <= maximum) {
            *value = (int)number;
            return 1;
        }
        printf("Enter a number from %d to %d.\n", minimum, maximum);
    }
}

static int ask_number_default(const char *prompt, int minimum, int maximum,
                              int default_value, int *value)
{
    char line[MAX_LINE];
    for (;;) {
        char *end;
        printf("%s", prompt);
        fflush(stdout);
        int result = read_line(line);
        if (result == READ_COMPLETED) {
            completion_menu();
            return 0;
        }
        if (result == 0) return 0;
        if (result > 0 && !line[0]) {
            *value = default_value;
            return 1;
        }
        long number = strtol(line, &end, 10);
        while (*end == ' ' || *end == '\t') end++;
        if (result > 0 && end != line && *end == '\0' &&
            number >= minimum && number <= maximum) {
            *value = (int)number;
            return 1;
        }
        printf("Enter a number from %d to %d, or press Enter for %d.\n",
               minimum, maximum, default_value);
    }
}

static int response_value(const char *response, const char *prefix,
                          char *value, size_t size)
{
    size_t prefix_length = strlen(prefix);
    if (strncmp(response, prefix, prefix_length)) return 0;
    const char *start = response + prefix_length;
    const char *end = strchr(start, '|');
    size_t length = end ? (size_t)(end - start) : strlen(start);
    if (!length || length >= size) return 0;
    memcpy(value, start, length);
    value[length] = '\0';
    return 1;
}

static int create_exam(void)
{
    char subject[64], command[MAX_LINE], response[MAX_LINE], exam_id[CODE_LEN];
    int seconds;
    if (!ask_text("Exam subject: ", subject, sizeof subject) ||
        !ask_number("Time limit in seconds: ", 1, 86400, &seconds))
        return 0;

    snprintf(command, sizeof command, "EXAM_CREATE|%s|%d", subject, seconds);
    if (!send_and_wait(command, response)) return 1;
    printf("\nAdd questions one at a time. Choose A, B, C, or D as the correct answer.\n");

    for (;;) {
        char text[256], options[4][128], correct[8];
        int marks, add_another;
        if (!ask_text("Question text: ", text, sizeof text)) return 0;
        if (!ask_text("Option A: ", options[0], sizeof options[0]) ||
            !ask_text("Option B: ", options[1], sizeof options[1]) ||
            !ask_text("Option C: ", options[2], sizeof options[2]) ||
            !ask_text("Option D: ", options[3], sizeof options[3]) ||
            !ask_text("Correct option (A-D): ", correct, sizeof correct))
            return 0;
        if (strlen(correct) != 1 || correct[0] < 'A' || correct[0] > 'D') {
            printf("The correct option must be one capital letter: A, B, C, or D.\n");
            continue;
        }
        if (!ask_number("Marks for this question: ", 1, 100000, &marks))
            return 0;

        snprintf(command, sizeof command, "EXAM_ADDQ|%s|%s|%s|%s|%s|%s|%d",
                 text, options[0], options[1], options[2], options[3], correct, marks);
        if (!send_and_wait(command, response)) return 1;
        if (!ask_number("Add another question? (1 = yes, 0 = no): ", 0, 1,
                        &add_another))
            return 0;
        if (!add_another) break;
    }

    if (!send_and_wait("EXAM_SAVE", response) ||
        !response_value(response, "OK|saved|", exam_id, sizeof exam_id))
        return 1;

    printf("\nExam %s has been saved. Opening a session now...\n", exam_id);
    snprintf(command, sizeof command, "EXAM_OPEN|%s", exam_id);
    if (send_and_wait(command, response)) {
        char session_code[CODE_LEN];
        if (response_value(response, "OK|session|", session_code, sizeof session_code))
            printf("\n*** Tell students to join with session code: %s ***\n\n",
                   session_code);
    }
    return 1;
}

static int create_quiz(void)
{
    int seconds, leaderboard_every;
    char command[MAX_LINE], response[MAX_LINE], room_code[CODE_LEN];
    if (!ask_number_default("Seconds per question (default 15): ", 1, 3600,
                            DEFAULT_QUESTION_SEC, &seconds) ||
        !ask_number_default("Show leaderboard every N questions (default 1): ",
                            1, MAX_QUESTIONS, DEFAULT_LEADERBOARD_EVERY,
                            &leaderboard_every))
        return 0;

    snprintf(command, sizeof command, "QUIZ_CREATE|%d|%d",
             seconds, leaderboard_every);
    if (!send_and_wait(command, response) ||
        !response_value(response, "OK|quiz_room|", room_code, sizeof room_code))
        return 1;

    printf("\nAdd quiz questions. Each player gets one answer per question.\n");
    for (;;) {
        char text[256], options[4][128], correct[8];
        int add_another;
        if (!ask_text("Question text: ", text, sizeof text) ||
            !ask_text("Option A: ", options[0], sizeof options[0]) ||
            !ask_text("Option B: ", options[1], sizeof options[1]) ||
            !ask_text("Option C: ", options[2], sizeof options[2]) ||
            !ask_text("Option D: ", options[3], sizeof options[3]) ||
            !ask_text("Correct option (A-D): ", correct, sizeof correct))
            return 0;
        char answer = (char)toupper((unsigned char)correct[0]);
        if (strlen(correct) != 1 || answer < 'A' || answer > 'D') {
            printf("The correct option must be A, B, C, or D.\n");
            continue;
        }

        snprintf(command, sizeof command, "QUIZ_ADDQ|%s|%s|%s|%s|%s|%c",
                 text, options[0], options[1], options[2], options[3], answer);
        if (!send_and_wait(command, response)) return 1;
        if (!ask_number("Add another question? (1 = yes, 0 = no): ",
                        0, 1, &add_another))
            return 0;
        if (!add_another) break;
    }

    printf("\n*** Quiz room %s is ready. Share this code with players. ***\n",
           room_code);
    printf("After players join, choose 'Start the live quiz' from the teacher menu.\n");
    return 1;
}

static int teacher_menu(void)
{
    char name[MAX_NAME], password[64], response[MAX_LINE];
    if (!ask_text("Teacher name: ", name, sizeof name) ||
        !ask_text("Admin password (default: admin123): ", password, sizeof password))
        return 0;

    char command[MAX_LINE];
    snprintf(command, sizeof command, "ADMIN_LOGIN|%s|%s", name, password);
    if (!send_and_wait(command, response)) return 1;

    for (;;) {
        int choice;
        printf("\n=== TEACHER MENU ===\n"
               "1. Create an exam (then open a student session)\n"
               "2. List saved exams\n"
               "3. Open a session for a saved exam\n"
               "4. Start the open session\n"
               "5. Create a live quiz\n"
               "6. Start the live quiz\n"
               "0. Exit\n");
        if (!ask_number("Choose an option: ", 0, 6, &choice)) return 0;

        if (choice == 0) return 1;
        if (choice == 1) {
            if (!create_exam()) return 0;
        } else if (choice == 2) {
            send_and_wait("EXAM_LIST", response);
        } else if (choice == 3) {
            char exam_id[CODE_LEN];
            send_and_wait("EXAM_LIST", response);
            if (!ask_text("Enter the exam ID to open: ", exam_id, sizeof exam_id))
                return 0;
            snprintf(command, sizeof command, "EXAM_OPEN|%s", exam_id);
            if (send_and_wait(command, response)) {
                char session_code[CODE_LEN];
                if (response_value(response, "OK|session|", session_code,
                                   sizeof session_code))
                    printf("\n*** Tell students to join with session code: %s ***\n",
                           session_code);
            }
        } else if (choice == 4) {
            printf("Students should join before you start. Their join notifications will appear here.\n");
            send_and_wait("EXAM_START", response);
        } else if (choice == 5) {
            if (!create_quiz()) return 0;
        } else if (choice == 6) {
            printf("Players should join before you start. Their join notifications will appear here.\n");
            send_and_wait("QUIZ_START", response);
        }
    }
}

static int student_exam_menu(void)
{
    char name[MAX_NAME], code[CODE_LEN], command[MAX_LINE], response[MAX_LINE];
    if (!ask_text("Your name: ", name, sizeof name) ||
        !ask_text("Session code from your teacher: ", code, sizeof code))
        return 0;

    snprintf(command, sizeof command, "EXAM_JOIN|%s|%s", code, name);
    if (!send_and_wait(command, response)) return 1;
    printf("You are in the session. Wait here for your teacher to start the exam.\n");
    printf("Questions will appear below when the exam begins. Keep this window open for your result.\n");

    for (;;) {
        int choice;
        printf("\n=== STUDENT MENU ===\n"
               "1. Answer or change an answer\n"
               "2. Submit the exam\n"
               "0. Exit\n");
        if (!ask_number("Choose an option: ", 0, 2, &choice)) return 0;
        if (choice == 0) return 1;
        if (choice == 1) {
            int question;
            char answer[8];
            if (!ask_number("Question number: ", 1, MAX_QUESTIONS, &question) ||
                !ask_text("Your answer (A, B, C, or D): ", answer, sizeof answer))
                return 0;
            if (strlen(answer) != 1 ||
                (answer[0] != 'A' && answer[0] != 'B' &&
                 answer[0] != 'C' && answer[0] != 'D')) {
                printf("Enter A, B, C, or D.\n");
                continue;
            }
            snprintf(command, sizeof command, "ANSWER|%d|%s", question, answer);
            send_and_wait(command, response);
        } else if (choice == 2) {
            send_and_wait("SUBMIT", response);
            printf("Your exam is submitted. Keep this window open for the results.\n");
        }
    }
}

static int student_quiz_menu(void)
{
    char name[MAX_NAME], code[CODE_LEN], command[MAX_LINE], response[MAX_LINE];
    if (!ask_text("Your name: ", name, sizeof name) ||
        !ask_text("Quiz room code from your teacher: ", code, sizeof code))
        return 0;

    snprintf(command, sizeof command, "QUIZ_JOIN|%s|%s", code, name);
    if (!send_and_wait(command, response)) return 1;
    printf("You joined the live quiz. Questions run for their full timer even after you answer.\n");
    printf("Correct answers are revealed when each timer ends. Keep this window open.\n");

    for (;;) {
        int choice;
        printf("\n=== LIVE QUIZ MENU ===\n"
               "1. Answer the current question\n"
               "0. Exit\n");
        if (!ask_number("Choose an option: ", 0, 1, &choice)) return 0;
        if (choice == 0) return 1;

        char answer[8];
        if (!ask_text("Your answer (A, B, C, or D): ", answer, sizeof answer))
            return 0;
        char option = (char)toupper((unsigned char)answer[0]);
        if (strlen(answer) != 1 || option < 'A' || option > 'D') {
            printf("Enter A, B, C, or D.\n");
            continue;
        }
        snprintf(command, sizeof command, "QUIZ_ANSWER|%c", option);
        send_and_wait(command, response);
    }
}

static int student_menu(void)
{
    int choice;
    printf("\n=== STUDENT MENU ===\n"
           "1. Join an exam\n"
           "2. Join a live quiz\n");
    if (!ask_number("Choose an option: ", 1, 2, &choice)) return 0;
    return choice == 1 ? student_exam_menu() : student_quiz_menu();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    socket_fd = net_connect(host, SERVER_PORT);
    if (socket_fd < 0) {
        perror("Could not connect to the server");
        printf("Start the server first, then run this client again.\n");
        return 1;
    }

    initialize_ui();
    if (pipe(wake_pipe) != 0) {
        perror("Could not create UI notification pipe");
        close(socket_fd);
        return 1;
    }

    pthread_t reader;
    int error = pthread_create(&reader, NULL, server_reader, NULL);
    if (error != 0) {
        fprintf(stderr, "Could not start server reader thread: %d\n", error);
        close(wake_pipe[0]);
        close(wake_pipe[1]);
        close(socket_fd);
        return 1;
    }

    pthread_mutex_lock(&response_lock);
    while (!connection_ready && connection_alive)
        pthread_cond_wait(&response_ready, &response_lock);
    int connected = connection_ready;
    pthread_mutex_unlock(&response_lock);
    if (!connected) {
        close(socket_fd);
        pthread_join(reader, NULL);
        return 1;
    }

    int keep_running = 1;
    while (keep_running) {
        int role;
        navigation_action = NAVIGATION_NONE;
        printf("\n=================================\n"
               "   EXAM SYSTEM - HOME\n"
               "=================================\n"
               "1. Teacher / admin\n"
               "2. Student\n"
               "0. Exit\n");
        int selected = ask_number("Choose an option: ", 0, 2, &role);
        if (navigation_action == NAVIGATION_EXIT) {
            keep_running = 0;
        } else if (navigation_action == NAVIGATION_HOME) {
            navigation_action = NAVIGATION_NONE;
        } else if (!selected || role == 0) {
            keep_running = 0;
        } else {
            int finished = role == 1 ? teacher_menu() : student_menu();
            if (navigation_action == NAVIGATION_EXIT)
                keep_running = 0;
            else if (navigation_action == NAVIGATION_HOME)
                navigation_action = NAVIGATION_NONE;
            else if (!finished)
                keep_running = 0;
            else
                keep_running = 0;
        }
    }

    printf("\nGoodbye.\n");
    shutdown(socket_fd, SHUT_RDWR);
    close(socket_fd);
    pthread_join(reader, NULL);
    close(wake_pipe[0]);
    close(wake_pipe[1]);
    return 0;
}
