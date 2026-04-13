#include <iostream>
#include <iomanip>
#include <string>
#include <queue>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include "shared.h"

using namespace std;

static int g_shmid = -1;
static int g_msgid = -1;
static SimClock* g_clk = nullptr;
static PCB g_table[TABLE_SIZE];
static FILE* g_logFile = nullptr;
static int g_totalResources[NUM_RESOURCES];
static int g_available[NUM_RESOURCES];
static int g_logLines = 0;

static int totalLaunched = 0;
static int activeChildren = 0;
static int nextLocalPid = 1;

static int totalRequests = 0;
static int grantedImmediately = 0;
static int deadlockRuns = 0;
static int deadlockKills = 0;

static bool verbose = true;

static void addTime(unsigned int addNS) {
    g_clk->nanoseconds += addNS;
    while (g_clk->nanoseconds >= BILLION) {
        g_clk->seconds++;
        g_clk->nanoseconds -= BILLION;
    }
}

static bool reachedTime(unsigned int s, unsigned int ns,
                        unsigned int ts, unsigned int tns) {
    return (s > ts) || (s == ts && ns >= tns);
}

static void logBoth(const char* fmt, ...) {
    va_list a1, a2;
    va_start(a1, fmt);
    va_copy(a2, a1);

    vprintf(fmt, a1);
    fflush(stdout);

    if (g_logFile && g_logLines < 10000) {
        vfprintf(g_logFile, fmt, a2);
        fflush(g_logFile);
        g_logLines++;
    }

    va_end(a2);
    va_end(a1);
}

static void cleanup() {
    if (g_msgid != -1) {
        msgctl(g_msgid, IPC_RMID, nullptr);
        g_msgid = -1;
    }
    if (g_clk && g_clk != (SimClock*)-1) {
        shmdt(g_clk);
        g_clk = nullptr;
    }
    if (g_shmid != -1) {
        shmctl(g_shmid, IPC_RMID, nullptr);
        g_shmid = -1;
    }
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

static void releaseAllResources(int i) {
    for (int r = 0; r < NUM_RESOURCES; r++) {
        g_available[r] += g_table[i].resourcesAllocated[r];
        g_table[i].resourcesAllocated[r] = 0;
    }
}

static void removePCB(int i) {
    g_table[i].occupied = 0;
    g_table[i].pid = 0;
    g_table[i].localPid = 0;
    g_table[i].blocked = 0;
    g_table[i].requestedResource = -1;
    for (int r = 0; r < NUM_RESOURCES; r++) {
        g_table[i].resourcesAllocated[r] = 0;
    }
}

static void signalHandler(int) {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].pid > 0) {
            kill(g_table[i].pid, SIGTERM);
        }
    }
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
    cleanup();
    _exit(1);
}

static int findFreeSlot() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (!g_table[i].occupied) return i;
    }
    return -1;
}

static void printTables() {
    logBoth("\nOSS PID:%d SysClock:%u:%u\n", getpid(), g_clk->seconds, g_clk->nanoseconds);
    logBoth("Process Table:\n");
    logBoth("Idx Occ PID Local Block Req ");
    for (int r = 0; r < NUM_RESOURCES; r++) logBoth("A%d ", r);
    logBoth("\n");

    for (int i = 0; i < TABLE_SIZE; i++) {
        logBoth("%2d  %d  %5d %5d %5d %3d ",
                i,
                g_table[i].occupied,
                g_table[i].pid,
                g_table[i].localPid,
                g_table[i].blocked,
                g_table[i].requestedResource);

        for (int r = 0; r < NUM_RESOURCES; r++) {
            logBoth("%2d ", g_table[i].resourcesAllocated[r]);
        }
        logBoth("\n");
    }

    logBoth("Available:\n");
    for (int r = 0; r < NUM_RESOURCES; r++) logBoth("R%d ", r);
    logBoth("\n");
    for (int r = 0; r < NUM_RESOURCES; r++) logBoth("%2d ", g_available[r]);
    logBoth("\nBlocked: ");
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].blocked) {
            logBoth("P%d(wait R%d) ", g_table[i].localPid, g_table[i].requestedResource);
        }
    }
    logBoth("\n\n");
}

static bool grantIfPossible(int i, int r) {
    if (r < 0 || r >= NUM_RESOURCES) return false;
    if (g_available[r] > 0) {
        g_available[r]--;
        g_table[i].resourcesAllocated[r]++;
        g_table[i].blocked = 0;
        g_table[i].requestedResource = -1;
        return true;
    }
    return false;
}

static void tryUnblockProcesses() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].blocked) {
            int r = g_table[i].requestedResource;
            if (grantIfPossible(i, r)) {
                Message wake;
                wake.mtype = g_table[i].pid;
                wake.index = i;
                wake.action = 999; // just a wake-up token
                wake.granted = r;
                msgsnd(g_msgid, &wake, sizeof(Message) - sizeof(long), 0);

                logBoth("Master unblocking P%d and granting R%d at time %u:%u\n",
                        g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);
            }
        }
    }
}

static bool detectDeadlock(bool deadlocked[]) {
    int work[NUM_RESOURCES];
    bool finish[TABLE_SIZE];

    for (int r = 0; r < NUM_RESOURCES; r++) work[r] = g_available[r];

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

static void resolveDeadlock() {
    bool deadlocked[TABLE_SIZE] = {false};
    deadlockRuns++;

    logBoth("Master running deadlock detection at time %u:%u\n",
            g_clk->seconds, g_clk->nanoseconds);

    if (!detectDeadlock(deadlocked)) {
        logBoth("No deadlock detected.\n");
        return;
    }

    logBoth("Deadlocked processes: ");
    int victim = -1;
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (deadlocked[i]) {
            logBoth("P%d ", g_table[i].localPid);
            if (victim == -1) victim = i;
        }
    }
    logBoth("\n");

    if (victim != -1) {
        logBoth("Killing process P%d to resolve deadlock.\n", g_table[victim].localPid);
        kill(g_table[victim].pid, SIGTERM);
        waitpid(g_table[victim].pid, nullptr, 0);
        releaseAllResources(victim);
        removePCB(victim);
        activeChildren--;
        deadlockKills++;
    }
}

int main(int argc, char* argv[]) {
    int n = 1;
    int s = 1;
    int t = 1;
    double launchInterval = 0.1;
    string logfile = "log.txt";

    int opt;
    while ((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch (opt) {
            case 'h':
                cout << "./oss [-h] [-n proc] [-s simul] [-t timeLimitForChildren] "
                     << "[-i fractionOfSecondToLaunchChildren] [-f logfile]\n";
                return 0;
            case 'n': n = atoi(optarg); break;
            case 's': s = atoi(optarg); break;
            case 't': t = atoi(optarg); break;
            case 'i': launchInterval = atof(optarg); break;
            case 'f': logfile = optarg; break;
            default: return 1;
        }
    }

    if (n < 1) n = 1;
    if (s < 1) s = 1;
    if (t < 1) t = 1;
    if (s > n) s = n;
    if (s > MAX_ACTIVE_PROCS) s = MAX_ACTIVE_PROCS;

    g_logFile = fopen(logfile.c_str(), "w");

    signal(SIGINT, signalHandler);
    signal(SIGALRM, signalHandler);
    alarm(5);

    key_t shmKey = ftok(".", 'C');
    g_shmid = shmget(shmKey, sizeof(SimClock), IPC_CREAT | 0666);
    g_clk = (SimClock*)shmat(g_shmid, nullptr, 0);
    g_clk->seconds = 0;
    g_clk->nanoseconds = 0;

    key_t msgKey = ftok(".", 'Q');
    g_msgid = msgget(msgKey, IPC_CREAT | 0666);

    memset(g_table, 0, sizeof(g_table));
    for (int i = 0; i < TABLE_SIZE; i++) g_table[i].requestedResource = -1;
    for (int r = 0; r < NUM_RESOURCES; r++) {
        g_totalResources[r] = INSTANCES_PER_RESOURCE;
        g_available[r] = INSTANCES_PER_RESOURCE;
    }

    unsigned int nextLaunchSec = 0;
    unsigned int nextLaunchNano = 0;
    unsigned int nextPrintSec = 0;
    unsigned int nextPrintNano = 500000000U;
    unsigned int nextDeadlockSec = 1;
    unsigned int nextDeadlockNano = 0;

    while (totalLaunched < n || activeChildren > 0) {
        while (waitpid(-1, nullptr, WNOHANG) > 0) {}

        if (totalLaunched < n &&
            activeChildren < s &&
            activeChildren < MAX_ACTIVE_PROCS &&
            reachedTime(g_clk->seconds, g_clk->nanoseconds, nextLaunchSec, nextLaunchNano)) {

            int slot = findFreeSlot();
            if (slot != -1) {
                unsigned int endSec = g_clk->seconds + (unsigned int)t;
                unsigned int endNano = g_clk->nanoseconds;

                pid_t pid = fork();
                if (pid == 0) {
                    char idxBuf[16], secBuf[32], nanoBuf[32];
                    snprintf(idxBuf, sizeof(idxBuf), "%d", slot);
                    snprintf(secBuf, sizeof(secBuf), "%u", endSec);
                    snprintf(nanoBuf, sizeof(nanoBuf), "%u", endNano);
                    execl("./worker", "worker", idxBuf, secBuf, nanoBuf, (char*)nullptr);
                    perror("execl worker");
                    _exit(1);
                } else if (pid > 0) {
                    g_table[slot].occupied = 1;
                    g_table[slot].pid = pid;
                    g_table[slot].localPid = nextLocalPid++;
                    g_table[slot].startSeconds = g_clk->seconds;
                    g_table[slot].startNano = g_clk->nanoseconds;
                    g_table[slot].endSeconds = endSec;
                    g_table[slot].endNano = endNano;
                    g_table[slot].blocked = 0;
                    g_table[slot].requestedResource = -1;
                    memset(g_table[slot].resourcesAllocated, 0, sizeof(g_table[slot].resourcesAllocated));

                    totalLaunched++;
                    activeChildren++;

                    double whole;
                    double frac = modf(launchInterval, &whole);
                    nextLaunchSec = g_clk->seconds + (unsigned int)whole;
                    nextLaunchNano = g_clk->nanoseconds + (unsigned int)(frac * BILLION);
                    while (nextLaunchNano >= BILLION) {
                        nextLaunchSec++;
                        nextLaunchNano -= BILLION;
                    }

                    logBoth("Launched P%d in slot %d pid %d at %u:%u ending at %u:%u\n",
                            g_table[slot].localPid, slot, pid,
                            g_clk->seconds, g_clk->nanoseconds, endSec, endNano);
                }
            }
        }

        tryUnblockProcesses();

        addTime(CLOCK_INCREMENT_NS);

        int picked = -1;
        for (int i = 0; i < TABLE_SIZE; i++) {
            if (g_table[i].occupied && !g_table[i].blocked) {
                picked = i;
                break;
            }
        }

        if (picked != -1) {
            Message msg;
            msg.mtype = g_table[picked].pid;
            msg.index = picked;
            msg.action = 999;
            msg.granted = -1;

            if (msgsnd(g_msgid, &msg, sizeof(Message) - sizeof(long), 0) == -1) {
                perror("msgsnd to worker");
                signalHandler(0);
            }

            Message reply;
            if (msgrcv(g_msgid, &reply, sizeof(Message) - sizeof(long), 1, 0) == -1) {
                perror("msgrcv from worker");
                signalHandler(0);
            }

            int i = reply.index;
            int action = reply.action;

            if (action > 0) {
                int r = action - 1;
                totalRequests++;
                logBoth("Master has detected P%d requesting R%d at time %u:%u\n",
                        g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);

                if (grantIfPossible(i, r)) {
                    grantedImmediately++;
                    logBoth("Master granting P%d request R%d at time %u:%u\n",
                            g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);
                } else {
                    g_table[i].blocked = 1;
                    g_table[i].requestedResource = r;
                    logBoth("Master blocking P%d for R%d at time %u:%u\n",
                            g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);
                }
            } else if (action < 0) {
                int r = (-action) - 1;
                if (r >= 0 && r < NUM_RESOURCES && g_table[i].resourcesAllocated[r] > 0) {
                    g_table[i].resourcesAllocated[r]--;
                    g_available[r]++;
                    logBoth("Master has acknowledged P%d releasing R%d at time %u:%u\n",
                            g_table[i].localPid, r, g_clk->seconds, g_clk->nanoseconds);
                }
            } else {
                logBoth("Master sees P%d terminating at time %u:%u\n",
                        g_table[i].localPid, g_clk->seconds, g_clk->nanoseconds);
                releaseAllResources(i);
                waitpid(g_table[i].pid, nullptr, 0);
                removePCB(i);
                activeChildren--;
            }
        }

        addTime(CLOCK_INCREMENT_NS);

        if (reachedTime(g_clk->seconds, g_clk->nanoseconds, nextPrintSec, nextPrintNano)) {
            printTables();
            nextPrintSec = g_clk->seconds;
            nextPrintNano = g_clk->nanoseconds + 500000000U;
            while (nextPrintNano >= BILLION) {
                nextPrintSec++;
                nextPrintNano -= BILLION;
            }
        }

        if (reachedTime(g_clk->seconds, g_clk->nanoseconds, nextDeadlockSec, nextDeadlockNano)) {
            resolveDeadlock();
            nextDeadlockSec = g_clk->seconds + 1;
            nextDeadlockNano = g_clk->nanoseconds;
        }
    }

    logBoth("\nFinal Statistics:\n");
    logBoth("Total requests: %d\n", totalRequests);
    logBoth("Granted immediately: %d\n", grantedImmediately);
    double pct = (totalRequests > 0) ? (100.0 * grantedImmediately / totalRequests) : 0.0;
    logBoth("Immediate grant percentage: %.2f%%\n", pct);
    logBoth("Deadlock detection runs: %d\n", deadlockRuns);
    logBoth("Processes killed for deadlock: %d\n", deadlockKills);

    cleanup();
    return 0;
}
