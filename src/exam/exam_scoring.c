#include "exam_session.h"
#include "net.h"

void exam_score_all(Session *session)
{
    for (int i = 0; i < session->count; i++) {
        Participant *participant = &session->participants[i];
        participant->score = 0;
        for (int q = 0; q < session->exam->num_questions; q++)
            if (participant->answers[q] == session->exam->questions[q].correct)
                participant->score += session->exam->questions[q].marks;
    }
}

int exam_rank_order(const Session *session, int indices[])
{
    for (int i = 0; i < session->count; i++) indices[i] = i;
    for (int i = 0; i < session->count; i++) {
        for (int j = 0; j + 1 < session->count - i; j++) {
            if (session->participants[indices[j]].score <
                session->participants[indices[j + 1]].score) {
                int temp = indices[j];
                indices[j] = indices[j + 1];
                indices[j + 1] = temp;
            }
        }
    }
    return session->count;
}

void exam_publish_results(Session *session)
{
    int indices[MAX_PARTICIPANTS];
    int total = exam_total_marks(session->exam);
    int count = exam_rank_order(session, indices);

    for (int rank = 0; rank < count; rank++) {
        Participant *participant = &session->participants[indices[rank]];
        if (participant->connected)
            send_line(participant->sockfd, "RESULT|%d|%d|%d", participant->score,
                      total, rank + 1);
    }

    for (int rank = 0; rank < count; rank++) {
        Participant *ranked = &session->participants[indices[rank]];
        if (session->admin_fd >= 0)
            send_line(session->admin_fd, "BOARD|%d|%s|%d|%d", rank + 1, ranked->name,
                      ranked->score, total);
        for (int i = 0; i < count; i++) {
            Participant *recipient = &session->participants[i];
            if (recipient->connected)
                send_line(recipient->sockfd, "BOARD|%d|%s|%d|%d", rank + 1,
                          ranked->name, ranked->score, total);
        }
    }

    if (session->admin_fd >= 0) send_line(session->admin_fd, "BOARD_END");
    for (int i = 0; i < count; i++)
        if (session->participants[i].connected)
            send_line(session->participants[i].sockfd, "BOARD_END");
}
