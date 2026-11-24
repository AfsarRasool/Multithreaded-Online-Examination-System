# Multithreaded Online Examination and Quiz System

A client-server application written in **C** using **POSIX threads (pthreads)** and **TCP sockets**. One server hosts two products at the same time:

- **Online Examination**: an examiner creates a timed exam, students join with a session code, answer (and change answers) until time ends, and receive scores and a ranked batch result.
- **Real-Time Quiz**: players join with just a nickname, answer timed questions one at a time, earn rank-based points, and see a leaderboard.

Course project for **CS3006 - Operating Systems**, FAST School of Computing, NUCES Karachi (Spring 2026).

---

## Table of Contents

1. [Features](#features)
2. [Operating System Concepts Demonstrated](#operating-system-concepts-demonstrated)
3. [Platform Support](#platform-support)
4. [Running on Windows with WSL](#running-on-windows-with-wsl)
5. [Build and Run](#build-and-run)
6. [How to Use: Exam Walkthrough](#how-to-use-exam-walkthrough)
7. [How to Use: Quiz Walkthrough](#how-to-use-quiz-walkthrough)
8. [Project Structure](#project-structure)
9. [Architecture](#architecture)
10. [Screenshots](#screenshots)
11. [Documentation](#documentation)
12. [Authors](#authors)

---

## Features

### Online Examination
- Admin (examiner) login with a shared password; the examiner's name is shown to every student.
- Subject-based exams stored as text files, plus custom exams created over the network.
- Multiple students join one session using a session code (joining closes once the exam starts).
- Students can **change answers any time until the exam ends**; the latest answer counts.
- Exam ends automatically when time expires, **or early when every student has submitted**.
- Scores calculated once, individual result and ranked batch table sent to everyone.
- Results appended to `data/results/results.txt`.
- Disconnected students keep their saved answers.

### Real-Time Quiz
- Join with a nickname only; no registration. Duplicate nicknames are rejected.
- One question at a time; each question runs for its **full time** (default **15 s**, configurable).
- **Rank-based scoring:** first correct answer 100 points, second 90, third 80 and so on, down to a minimum of 50. Correct answers in the **final 5 seconds** earn a fixed 50.
- Correct answer is revealed after each question.
- Leaderboard shown every **N** questions (default every question, configurable by the admin).
- Final leaderboard sent to all players and saved to file.

### Concurrency
- Many clients at once, with several exam sessions and quiz rooms running independently.
- The server is the only authority for time, scoring and ranking.

---

## Operating System Concepts Demonstrated

| Concept | Where it appears |
|---|---|
| Multithreading | One thread per client, one timer thread per running exam, one coordinator per running quiz |
| Socket programming | TCP client-server communication, line-based text protocol |
| Mutual exclusion (mutex) | Participant lists, answers, scores, quiz rank counter, results file |
| Critical-section protection | Check-state-and-write-answer done under a single lock |
| Condition variables | Timer thread sleeps until the deadline or until all students submit |
| Thread coordination | Session states `WAITING -> RUNNING -> FINISHED`, timers, early completion |

**The key race condition:** when the exam timer expires at the same instant a student's answer arrives, the state check and the answer write happen inside **one** lock hold, and the timer takes the same lock to finish the exam. An answer is therefore either fully counted or cleanly rejected, never half-counted.

---

## Platform Support

| Platform | Supported |
|---|---|
| Linux (Ubuntu, Debian, Fedora, ...) | Yes |
| **Windows via WSL** (Ubuntu) | **Yes (recommended on Windows)** |
| macOS | Should work (POSIX), not tested |
| Native Windows (MinGW/MSVC) | **No**: the code uses POSIX headers (`pthread.h`, `sys/socket.h`, `poll.h`) |

**Requirements:** `gcc`, `make`, and a POSIX system.

---

## Running on Windows with WSL

WSL (Windows Subsystem for Linux) gives you a real Linux environment inside Windows.

1. Open **PowerShell as Administrator** and run:
   ```powershell
   wsl --install
   ```
   Restart the computer if asked. This installs Ubuntu by default.
2. Open the **Ubuntu** app from the Start menu and create a username and password.
3. Install the build tools:
   ```bash
   sudo apt update
   sudo apt install build-essential git
   ```
4. Clone the repository **inside the Linux file system** (e.g. your home folder):
   ```bash
   cd ~
   git clone <YOUR-REPOSITORY-URL>
   cd <repository-folder>
   ```
5. Continue with [Build and Run](#build-and-run).

Notes for WSL:
- Open **several Ubuntu terminals** (one for the server, others for clients). In Windows Terminal use the `+` button or `Ctrl+Shift+T`.
- On WSL2, clients on the same PC connect with `127.0.0.1` (the default).
- To connect from a **different computer**, the server must be reachable on the network. Find the address with `hostname -I` and, on WSL2, you may need to allow port 5555 through the Windows firewall or enable mirrored networking. For demos, running everything on one PC is simplest.

---

## Build and Run

### 1. Build
```bash
make
```
This creates two executables, `server` and `client`. To clean up: `make clean`.

### 2. Start the server
**Run it from the project root folder**, because it reads and writes the relative `data/` folders.
```bash
./server
```
Expected output:
```
[server] loaded N exam(s)
[server] listening on port 5555
```

### 3. Start clients (one terminal each)
```bash
./client                 # server on the same machine
./client 192.168.1.20    # server on another machine (use its IP)
```
The client sends whatever you type and prints every server message starting with `<`. The default port is **5555** (change `SERVER_PORT` in `include/common.h`).

### Default admin password
`admin123`, set by `ADMIN_PASSWORD` in `include/common.h`. Change it before real use.

---

## How to Use: Exam Walkthrough

Messages have the form `COMMAND|arg1|arg2|...`. Open three terminals running `./client`: one examiner and two students.

**Examiner terminal**
```
ADMIN_LOGIN|Sir Sandesh|admin123
EXAM_CREATE|Operating Systems|120
EXAM_ADDQ|What does a mutex provide?|Mutual exclusion|Paging|Caching|Routing|A|2
EXAM_ADDQ|Which call creates a thread?|fork|pthread_create|exec|wait|B|3
EXAM_SAVE                      -> OK|saved|E1
EXAM_OPEN|E1                   -> OK|session|S1001
```

**Student terminals** (use the session code you received)
```
EXAM_JOIN|S1001|Ali
EXAM_JOIN|S1001|Sara
```
The examiner sees `EVT|JOINED|...` for each student.

**Examiner starts the exam**
```
EXAM_START
```

**Students answer** (answers can be changed until the exam ends)
```
ANSWER|1|A
ANSWER|2|B
ANSWER|1|C        <- changes the answer to question 1
SUBMIT
```

The exam ends when time runs out or all students have submitted. Everyone receives the ranked table (`BOARD|rank|name|score|total`, then `BOARD_END`) and each student receives `RESULT|score|total|rank`.

### Exam command reference

| Command | Who | Description |
|---|---|---|
| `ADMIN_LOGIN\|name\|password` | Admin | Authenticate as examiner |
| `EXAM_CREATE\|subject\|seconds` | Admin | Start a new exam draft |
| `EXAM_ADDQ\|text\|A\|B\|C\|D\|correct\|marks` | Admin | Add a question (`correct` is A-D) |
| `EXAM_SAVE` | Admin | Save the draft; returns the exam id |
| `EXAM_LIST` | Anyone | List saved exams |
| `EXAM_OPEN\|examId` | Admin | Open a session; returns the session code |
| `EXAM_START` | Admin | Start the exam for all joined students |
| `EXAM_JOIN\|code\|name` | Student | Join a session while it is waiting |
| `ANSWER\|q\|X` | Student | Save or change the answer to question `q` |
| `SUBMIT` | Student | Declare you are finished |
| `PING`, `WHOAMI` | Anyone | Connection test and current role |

Saved exams live in `data/exams/` as plain text, so you can also write them by hand:
```
ID|E1
SUBJECT|Operating Systems
ADMIN|Sir Sandesh
TIME|600
Q|What does a mutex provide?|Mutual exclusion|Paging|Caching|Routing|A|2
```

---

## How to Use: Quiz Walkthrough

<!-- TODO (authors): fill this section with the exact commands from your quiz implementation. -->

1. The admin logs in with `ADMIN_LOGIN|name|password` and creates a quiz room, choosing the question time (default 15 s) and how often the leaderboard appears (default every question). The server returns a room code.
2. Players join with the room code and a nickname (no registration).
3. The admin starts the quiz. Each question is sent to all players and runs for its full time.
4. After each question the correct answer is shown, and the leaderboard appears every N questions.
5. The final leaderboard is sent to everyone and saved to file.

| Command | Who | Description |
|---|---|---|
| `QUIZ_...` | Admin | *(fill in: create room)* |
| `QUIZ_...` | Admin | *(fill in: start quiz)* |
| `QUIZ_...` | Player | *(fill in: join with nickname)* |
| `QUIZ_...` | Player | *(fill in: answer)* |

---

## Project Structure

```
os-project/
├── Makefile
├── README.md
├── doc/                      Final report and project proposal
├── include/                  Header files
│   ├── common.h              Constants (port, limits, admin password, quiz defaults)
│   ├── net.h                 Socket helpers
│   ├── protocol.h            Message parsing
│   ├── exam.h                Exam template structures
│   ├── exam_session.h        Session and participant structures, session API
│   ├── quiz.h                Quiz room structures
│   └── results.h             Results saving
├── src/
│   ├── common/
│   │   ├── net.c             listen/accept/connect, send_line, recv_line
│   │   └── protocol.c        Splits "COMMAND|arg|arg" into fields
│   ├── exam/
│   │   ├── exam.c            Load/save/list/find exam templates
│   │   ├── exam_session.c    Create, join, start, answer, submit, leave (all locking here)
│   │   ├── exam_timer.c      Timer thread (condition variable with timeout)
│   │   └── exam_scoring.c    Scoring, ranking, result broadcast
│   ├── quiz/                 Quiz rooms, rounds, rank-based scoring, leaderboard
│   ├── results/
│   │   └── results.c         Appends results to file under a mutex
│   ├── server/
│   │   ├── main.c            Accept loop, one thread per client
│   │   └── client_handler.c  Per-client thread and command dispatcher
│   └── client/
│       └── client_main.c     Terminal client (poll on keyboard + socket)
└── data/
    ├── exams/                Saved exams (*.txt)
    └── results/              Saved results
```

Each module hides its own locking: outside code calls functions and never touches a mutex directly.

---

## Architecture

```
 Examiner client ──┐
 Student clients ──┤                 ┌─ client thread (one per connection)
 Quiz players    ──┼── TCP sockets ──┼─ exam timer thread (one per running exam)
 Quiz admin      ──┘                 └─ quiz coordinator thread (one per running room)
                                              │
                                     SHARED STATE (mutex-protected)
                                     sessions, participants, answers, scores, results
```

| Shared data | Protection |
|---|---|
| Exam and session registries | Registry mutexes |
| Participant list, answers, scores, session state | One mutex per session |
| Quiz rank counter and scores | One mutex per room |
| Results file | File mutex |

Layers: `server` (connections and routing) -> `exam` / `quiz` / `results` (features, independent of each other) -> `common` (sockets and protocol).

---

## Screenshots

> Add your screenshots to a folder such as `doc/images/` and update the paths below.

### Server running
<!-- ![Server running](doc/images/server.png) -->
*Screenshot placeholder*

### Examiner creating and starting an exam
<!-- ![Examiner](doc/images/examiner.png) -->
*Screenshot placeholder*

### Students answering during an exam
<!-- ![Students](doc/images/students.png) -->
*Screenshot placeholder*

### Exam results table
<!-- ![Exam results](doc/images/exam-results.png) -->
*Screenshot placeholder*

### Quiz in progress
<!-- ![Quiz](doc/images/quiz.png) -->
*Screenshot placeholder*

### Quiz leaderboard
<!-- ![Leaderboard](doc/images/leaderboard.png) -->
*Screenshot placeholder*

---

## Documentation

The `doc/` folder contains:
- **Project Proposal**
- **Final Report** (objectives, project details, results, conclusion)

---

## Limitations

Terminal clients only, a single shared admin password, plain-text (unencrypted) communication, and live state kept in memory (results are saved to files when a session ends).

---

## Authors

| Name | Roll Number |
|---|---|
| Afsar Rasool | 24K-2016 |
| Abu Bakar | 24K-2046 |

**Section:** CY-5A  
**Instructor:** Sandesh Kumar  
**Course:** CS3006 - Operating Systems, Spring 2026  
National University of Computer and Emerging Sciences (FAST-NUCES), Karachi
