#include <iostream>
#include <iomanip>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <cmath>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include "shared.h"

using namespace std;

static int g_shmid = -1;
static int g_msgid = -1;
static SimClock* g_clk = nullptr;
static PCB g_table[TABLE_SIZE];
static FILE* g_logFile = nullptr;

static int g_available[NUM_RESOURCES];
static int g_total[NUM_RESOURCES];

static int g_logLines = 0;
static const int LOG_LIMIT = 10000;

static int g_totalRequests = 0;
static int g_immediateGrants = 0;
static int g_deadlockRuns = 0;
static int g_deadlockKills = 0;

static bool g_verbose = true;

static void logBoth(const char* fmt, ...) {
    va_list args1, args2;
    va_start(args1, fmt);
    va_copy(args2, args1);

    vprintf(fmt, args1);
    fflush(stdout);

    if (g_logFile && g_logLines < LOG_LIMIT) {
        vfprintf(g_logFile, fmt, args2);
        fflush(g_logFile);
        g_logLines++;
    }

    va_end(args2);
    va_end(args1);
}

static void addToClock(unsigned int addNS) {
    if (!g_clk) return;

    g_clk->nanoseconds += addNS;
    while (g_clk->nanoseconds >= BILLION) {
        g_clk->seconds++;
        g_clk->nanoseconds -= BILLION;
    }
}

static bool timeGTE(unsigned int sA, unsigned int nA,
                    unsigned int sB, unsigned int nB) {
    return (sA > sB) || (sA == sB && nA >= nB);
}

static void addTimeToPair(unsigned int baseS, unsigned int baseNS,
                          unsigned int addNS,
                          unsigned int& outS, unsigned int& outNS) {
    unsigned long long total = (unsigned long long)baseNS + addNS;
    outS = baseS + (unsigned int)(total / BILLION);
    outNS = (unsigned int)(total % BILLION);
}

static void cleanup() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].pid > 0) {
            kill(g_table[i].pid, SIGTERM);
        }
    }

    while (waitpid(-1, nullptr, WNOHANG) > 0) {}

    if (g_clk && g_clk != (SimClock*)-1) {
        shmdt(g_clk);
        g_clk = nullptr;
    }

    if (g_shmid != -1) {
        shmctl(g_shmid, IPC_RMID, nullptr);
        g_shmid = -1;
    }

    if (g_msgid != -1) {
        msgctl(g_msgid, IPC_RMID, nullptr);
        g_msgid = -1;
    }

    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

static void signal_handler(int) {
    cleanup();
    _exit(1);
}

static void printHelp(const char* prog) {
    cout << "Usage: " << prog
         << " [-h] [-n proc] [-s simul] [-t timeLimitForChildren] "
         << "[-i fractionOfSecondToLaunchChildren] [-f logfile]\n";
}

static int findFreeSlot() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (!g_table[i].occupied) return i;
    }
    return -1;
}

static void clearPCB(int i) {
    g_table[i].occupied = 0;
    g_table[i].pid = 0;
    g_table[i].localPid = 0;
    g_table[i].startSeconds = 0;
    g_table[i].startNano = 0;
    g_table[i].endSeconds = 0;
    g_table[i].endNano = 0;
    g_table[i].blocked = 0;
    g_table[i].requestedResource = -1;
    for (int r = 0; r < NUM_RESOURCES; r++) {
        g_table[i].resourcesAllocated[r] = 0;
    }
}

static void releaseAllResources(int i) {
    for (int r = 0; r < NUM_RESOURCES; r++) {
        g_available[r] += g_table[i].resourcesAllocated[r];
        g_table[i].resourcesAllocated[r] = 0;
    }
}

static bool grantResource(int i, int r) {
    if (r < 0 || r >= NUM_RESOURCES) return false;
    if (g_available[r] <= 0) return false;

    g_available[r]--;
    g_table[i].resourcesAllocated[r]++;
    g_table[i].blocked = 0;
    g_table[i].requestedResource = -1;
    return true;
}

static void printBlockedList() {
    string line = "Blocked queue [";
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].blocked) {
            line += " P" + to_string(g_table[i].localPid)
                 + "(R" + to_string(g_table[i].requestedResource) + ")";
        }
    }
    line += " ]\n";
    logBoth("%s", line.c_str());
}

static void printResourceTable() {
    logBoth("Total resources:\n");
    for (int r = 0; r < NUM_RESOURCES; r++) logBoth("R%-2d ", r);
    logBoth("\n");
    for (int r = 0; r < NUM_RESOURCES; r++) logBoth("%-3d ", g_total[r]);
    logBoth("\n");

    logBoth("Available resources:\n");
    for (int r = 0; r < NUM_RESOURCES; r++) logBoth("R%-2d ", r);
    logBoth("\n");
    for (int r = 0; r < NUM_RESOURCES; r++) logBoth("%-3d ", g_available[r]);
    logBoth("\n");

    logBoth("Allocated resources per process:\n");
    logBoth("Idx LocalPID Blk Req  ");
    for (int r = 0; r < NUM_RESOURCES; r++) logBoth("R%-2d ", r);
    logBoth("\n");

    for (int i = 0; i < TABLE_SIZE; i++) {
        PCB& p = g_table[i];
        logBoth("%-3d %-7d %-3d %-3d ",
                i,
                p.localPid,
                p.blocked,
                p.requestedResource);

        for (int r = 0; r < NUM_RESOURCES; r++) {
            logBoth("%-3d ", p.resourcesAllocated[r]);
        }
        logBoth("\n");
    }
}

static void printProcessTable() {
    logBoth("\nOSS PID:%d SysClockS:%u SysClockNano:%u\n",
            getpid(), g_clk->seconds, g_clk->nanoseconds);
    logBoth("Process Table:\n");
    logBoth("Entry Occupied PID LocalPID StartS StartN EndS EndN Blocked Req\n");

    for (int i = 0; i < TABLE_SIZE; i++) {
        PCB& p = g_table[i];
        logBoth("%d %d %d %d %u %u %u %u %d %d\n",
                i,
                p.occupied ? 1 : 0,
                (int)p.pid,
                p.localPid,
                p.startSeconds,
                p.startNano,
                p.endSeconds,
                p.endNano,
                p.blocked ? 1 : 0,
                p.requestedResource);
    }

    printBlockedList();
    printResourceTable();
    logBoth("\n");
}

static void tryUnblockProcesses() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].blocked) {
            int r = g_table[i].requestedResource;
            if (r >= 0 && r < NUM_RESOURCES && g_available[r] > 0) {
                grantResource(i, r);

                Message msg;
                msg.mtype = g_table[i].pid;
                msg.index = i;
                msg.action = 999;   // wake token / run token
                msg.granted = r;    // tell worker this blocked request was granted

                if (msgsnd(g_msgid, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
                    cerr << "OSS: msgsnd unblock failed: " << strerror(errno) << "\n";
                    cleanup();
                    exit(1);
                }

                if (g_verbose) {
                    logBoth("Master unblocking P%d and granting R%d at time %u:%u\n",
                            g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);
                }
            }
        }
    }
}

static bool detectDeadlock(bool deadlocked[]) {
    int work[NUM_RESOURCES];
    bool finish[TABLE_SIZE];

    for (int r = 0; r < NUM_RESOURCES; r++) {
        work[r] = g_available[r];
    }

    for (int i = 0; i < TABLE_SIZE; i++) {
        if (!g_table[i].occupied) finish[i] = true;
        else if (!g_table[i].blocked) finish[i] = true;
        else finish[i] = false;
    }

    bool changed;
    do {
        changed = false;
        for (int i = 0; i < TABLE_SIZE; i++) {
            if (!finish[i] && g_table[i].occupied && g_table[i].blocked) {
                int req = g_table[i].requestedResource;
                if (req >= 0 && req < NUM_RESOURCES && work[req] > 0) {
                    finish[i] = true;
                    for (int r = 0; r < NUM_RESOURCES; r++) {
                        work[r] += g_table[i].resourcesAllocated[r];
                    }
                    changed = true;
                }
            }
        }
    } while (changed);

    bool found = false;
    for (int i = 0; i < TABLE_SIZE; i++) {
        deadlocked[i] = (!finish[i] && g_table[i].occupied && g_table[i].blocked);
        if (deadlocked[i]) found = true;
    }

    return found;
}

static void resolveDeadlock(int& activeChildren) {
    bool deadlocked[TABLE_SIZE] = {false};
    g_deadlockRuns++;

    logBoth("Master running deadlock detection at time %u:%u\n",
            g_clk->seconds, g_clk->nanoseconds);

    bool found = detectDeadlock(deadlocked);

    if (!found) {
        logBoth("No deadlock detected.\n");
        return;
    }

    logBoth("Deadlocked processes:");
    int victim = -1;
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (deadlocked[i]) {
            logBoth(" P%d", g_table[i].localPid);
            if (victim == -1) victim = i;
        }
    }
    logBoth("\nAttempting to resolve deadlock...\n");

    if (victim != -1) {
        logBoth("Killing process P%d:\n", g_table[victim].localPid);
        logBoth("Resources released are as follows: ");
        for (int r = 0; r < NUM_RESOURCES; r++) {
            if (g_table[victim].resourcesAllocated[r] > 0) {
                logBoth("R%d:%d ", r, g_table[victim].resourcesAllocated[r]);
            }
        }
        logBoth("\n");

        kill(g_table[victim].pid, SIGTERM);
        waitpid(g_table[victim].pid, nullptr, 0);
        releaseAllResources(victim);
        clearPCB(victim);
        activeChildren--;
        g_deadlockKills++;
    }
}

static int pickRunnableProcess() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && !g_table[i].blocked) {
            return i;
        }
    }
    return -1;
}

static unsigned int secondsPart(double x) {
    if (x <= 0.0) return 0;
    return (unsigned int)floor(x);
}

static unsigned int nanosPart(double x) {
    if (x <= 0.0) return 0;
    double whole = floor(x);
    double frac = x - whole;
    return (unsigned int)(frac * 1000000000.0);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGALRM, signal_handler);
    alarm(5);

    srand((unsigned int)(time(nullptr) ^ getpid()));

    int n = 1;
    int s = 1;
    double t = 2.0;
    double interval = 0.1;
    string logFilename = "log.txt";

    int opt;
    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                printHelp(argv[0]);
                return 0;
            case 'n':
                n = atoi(optarg);
                if (n <= 0) {
                    cerr << "Error: -n must be > 0\n";
                    return 1;
                }
                break;
            case 's':
                s = atoi(optarg);
                if (s <= 0) {
                    cerr << "Error: -s must be > 0\n";
                    return 1;
                }
                break;
            case 't':
                t = atof(optarg);
                if (t <= 0.0) {
                    cerr << "Error: -t must be > 0\n";
                    return 1;
                }
                break;
            case 'i':
                interval = atof(optarg);
                if (interval < 0.0) {
                    cerr << "Error: -i must be >= 0\n";
                    return 1;
                }
                break;
            case 'f':
                logFilename = optarg;
                break;
            default:
                printHelp(argv[0]);
                return 1;
        }
    }

    if (s > n) s = n;
    if (s > MAX_ACTIVE_PROCS) s = MAX_ACTIVE_PROCS;

    g_logFile = fopen(logFilename.c_str(), "w");
    if (!g_logFile) {
        cerr << "OSS: failed to open log file: " << logFilename << "\n";
        return 1;
    }

    key_t shmKey = ftok(".", 'C');
    if (shmKey == -1) {
        cerr << "OSS: ftok shared memory failed: " << strerror(errno) << "\n";
        cleanup();
        return 1;
    }

    g_shmid = shmget(shmKey, sizeof(SimClock), 0666 | IPC_CREAT);
    if (g_shmid == -1) {
        cerr << "OSS: shmget failed: " << strerror(errno) << "\n";
        cleanup();
        return 1;
    }

    g_clk = (SimClock*)shmat(g_shmid, nullptr, 0);
    if (g_clk == (SimClock*)-1) {
        cerr << "OSS: shmat failed: " << strerror(errno) << "\n";
        cleanup();
        return 1;
    }

    g_clk->seconds = 0;
    g_clk->nanoseconds = 0;

    key_t msgKey = ftok(".", 'Q');
    if (msgKey == -1) {
        cerr << "OSS: ftok message queue failed: " << strerror(errno) << "\n";
        cleanup();
        return 1;
    }

    g_msgid = msgget(msgKey, 0666 | IPC_CREAT);
    if (g_msgid == -1) {
        cerr << "OSS: msgget failed: " << strerror(errno) << "\n";
        cleanup();
        return 1;
    }

    memset(g_table, 0, sizeof(g_table));
    for (int i = 0; i < TABLE_SIZE; i++) {
        g_table[i].requestedResource = -1;
    }

    for (int r = 0; r < NUM_RESOURCES; r++) {
        g_total[r] = INSTANCES_PER_RESOURCE;
        g_available[r] = INSTANCES_PER_RESOURCE;
    }

    int launched = 0;
    int activeChildren = 0;
    int nextLocalPid = 1;

    unsigned int nextLaunchS = 0;
    unsigned int nextLaunchNS = 0;

    unsigned int nextPrintS = 0;
    unsigned int nextPrintNS = 500000000U;

    unsigned int nextDeadlockS = 1;
    unsigned int nextDeadlockNS = 0;

    unsigned int intervalSec = secondsPart(interval);
    unsigned int intervalNano = nanosPart(interval);

    while (launched < n || activeChildren > 0) {
        while (true) {
            int status = 0;
            pid_t dead = waitpid(-1, &status, WNOHANG);
            if (dead <= 0) break;

            for (int i = 0; i < TABLE_SIZE; i++) {
                if (g_table[i].occupied && g_table[i].pid == dead) {
                    releaseAllResources(i);
                    clearPCB(i);
                    activeChildren--;
                    break;
                }
            }
        }

        if (launched < n &&
            activeChildren < s &&
            activeChildren < MAX_ACTIVE_PROCS &&
            timeGTE(g_clk->seconds, g_clk->nanoseconds, nextLaunchS, nextLaunchNS)) {

            int slot = findFreeSlot();
            if (slot != -1) {
                unsigned int endS = g_clk->seconds + (unsigned int)t;
                unsigned int endNS = g_clk->nanoseconds;

                pid_t child = fork();
                if (child == 0) {
                    string idxStr = to_string(slot);
                    string secStr = to_string(endS);
                    string nanoStr = to_string(endNS);

                    execl("./worker", "worker",
                          idxStr.c_str(),
                          secStr.c_str(),
                          nanoStr.c_str(),
                          (char*)nullptr);

                    cerr << "OSS: execl failed: " << strerror(errno) << "\n";
                    _exit(1);
                } else if (child > 0) {
                    g_table[slot].occupied = 1;
                    g_table[slot].pid = child;
                    g_table[slot].localPid = nextLocalPid++;
                    g_table[slot].startSeconds = g_clk->seconds;
                    g_table[slot].startNano = g_clk->nanoseconds;
                    g_table[slot].endSeconds = endS;
                    g_table[slot].endNano = endNS;
                    g_table[slot].blocked = 0;
                    g_table[slot].requestedResource = -1;
                    for (int r = 0; r < NUM_RESOURCES; r++) {
                        g_table[slot].resourcesAllocated[r] = 0;
                    }

                    launched++;
                    activeChildren++;

                    unsigned int tempS = g_clk->seconds + intervalSec;
                    unsigned int tempNS = g_clk->nanoseconds + intervalNano;
                    while (tempNS >= BILLION) {
                        tempS++;
                        tempNS -= BILLION;
                    }
                    nextLaunchS = tempS;
                    nextLaunchNS = tempNS;

                    logBoth("OSS: Generating process with local PID %d in slot %d at time %u:%u\n",
                            g_table[slot].localPid, slot,
                            g_clk->seconds, g_clk->nanoseconds);
                } else {
                    cerr << "OSS: fork failed: " << strerror(errno) << "\n";
                }
            }
        }

        tryUnblockProcesses();

        addToClock(CLOCK_INCREMENT_NS);

        int picked = pickRunnableProcess();
        if (picked != -1) {
            Message msg;
            msg.mtype = g_table[picked].pid;
            msg.index = picked;
            msg.action = 999;   // permission to run / act
            msg.granted = -1;

            if (msgsnd(g_msgid, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
                cerr << "OSS: msgsnd failed: " << strerror(errno) << "\n";
                cleanup();
                return 1;
            }

            Message reply;
            if (msgrcv(g_msgid, &reply, sizeof(Message) - sizeof(long), 1, 0) == -1) {
                cerr << "OSS: msgrcv failed: " << strerror(errno) << "\n";
                cleanup();
                return 1;
            }

            int i = reply.index;
            int action = reply.action;

            if (action > 0) {
                int r = action - 1;
                g_totalRequests++;

                if (g_verbose) {
                    logBoth("Master has detected Process P%d requesting R%d at time %u:%u\n",
                            g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);
                }

                if (grantResource(i, r)) {
                    g_immediateGrants++;

                    if (g_verbose) {
                        logBoth("Master granting P%d request R%d at time %u:%u\n",
                                g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);
                    }

                    Message ack;
                    ack.mtype = g_table[i].pid;
                    ack.index = i;
                    ack.action = 999;
                    ack.granted = r;

                    if (msgsnd(g_msgid, &ack, sizeof(Message) - sizeof(long), 0) == -1) {
                        cerr << "OSS: immediate grant ack msgsnd failed: " << strerror(errno) << "\n";
                        cleanup();
                        return 1;
                    }
                } else {
                    g_table[i].blocked = 1;
                    g_table[i].requestedResource = r;

                    if (g_verbose) {
                        logBoth("Master cannot grant P%d request R%d at time %u:%u, blocking process\n",
                                g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);
                    }
                }
            }
            else if (action < 0) {
                int r = (-action) - 1;

                if (r >= 0 && r < NUM_RESOURCES && g_table[i].resourcesAllocated[r] > 0) {
                    g_table[i].resourcesAllocated[r]--;
                    g_available[r]++;

                    if (g_verbose) {
                        logBoth("Master has acknowledged Process P%d releasing R%d at time %u:%u\n",
                                g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);
                    }
                }
            }
            else {
                logBoth("Master sees Process P%d terminating at time %u:%u\n",
                        g_table[i].localPid, g_clk->seconds, g_clk->nanoseconds);

                releaseAllResources(i);
                waitpid(g_table[i].pid, nullptr, 0);
                clearPCB(i);
                activeChildren--;
            }
        }

        addToClock(CLOCK_INCREMENT_NS);

        if (timeGTE(g_clk->seconds, g_clk->nanoseconds, nextPrintS, nextPrintNS)) {
            printProcessTable();
            nextPrintS = g_clk->seconds;
            nextPrintNS = g_clk->nanoseconds + 500000000U;
            while (nextPrintNS >= BILLION) {
                nextPrintS++;
                nextPrintNS -= BILLION;
            }
        }

        if (timeGTE(g_clk->seconds, g_clk->nanoseconds, nextDeadlockS, nextDeadlockNS)) {
            resolveDeadlock(activeChildren);
            nextDeadlockS = g_clk->seconds + 1;
            nextDeadlockNS = g_clk->nanoseconds;
        }
    }

    logBoth("\nOSS Summary:\n");
    logBoth("Total resources requested: %d\n", g_totalRequests);
    logBoth("Deadlock detection runs: %d\n", g_deadlockRuns);
    logBoth("Processes terminated due to deadlock: %d\n", g_deadlockKills);

    double pct = 0.0;
    if (g_totalRequests > 0) {
        pct = ((double)g_immediateGrants / (double)g_totalRequests) * 100.0;
    }

    logBoth("Immediate grants: %d\n", g_immediateGrants);
    logBoth("Immediate grant percentage: %.2f%%\n", pct);

    cleanup();
    return 0;
}
