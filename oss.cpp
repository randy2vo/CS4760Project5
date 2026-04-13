#include <iostream>
#include <string>
#include <queue>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <csignal>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include "shared.h"

using namespace std;

static int g_shmid = -1;
static int g_msgid = -1;
static SimClock* g_clk = nullptr;
static PCB g_table[TABLE_SIZE];
static FILE* g_logFile = nullptr;

static queue<int> readyQueue;

static int g_logLines = 0;
static const int LOG_LIMIT = 10000;

// statistics
static unsigned long long g_totalBusyTime = 0;      // time used by workers
static unsigned long long g_totalOverheadTime = 0;  // oss overhead time

static void logBoth(const char* fmt, ...) {
    if (g_logLines >= LOG_LIMIT) return;

    va_list args1, args2;
    va_start(args1, fmt);
    va_copy(args2, args1);

    vprintf(fmt, args1);
    fflush(stdout);

    if (g_logFile) {
        vfprintf(g_logFile, fmt, args2);
        fflush(g_logFile);
    }

    va_end(args2);
    va_end(args1);
    g_logLines++;
}

static void addToClock(unsigned int addNS) {
    if (!g_clk) return;

    g_clk->nanoseconds += addNS;
    while (g_clk->nanoseconds >= BILLION) {
        g_clk->seconds++;
        g_clk->nanoseconds -= BILLION;
    }
}

static void addOverhead(unsigned int ns) {
    addToClock(ns);
    g_totalOverheadTime += ns;
}

static bool timeGTE(unsigned int sA, unsigned int nA,
                    unsigned int sB, unsigned int nB) {
    return (sA > sB) || (sA == sB && nA >= nB);
}

static long long nanosBetween(unsigned int s1, unsigned int n1,
                              unsigned int s2, unsigned int n2) {
    return (long long)(s2 - s1) * 1000000000LL + (long long)n2 - (long long)n1;
}

static void addTimeToPair(unsigned int baseS, unsigned int baseNS,
                          unsigned int addNS,
                          unsigned int& outS, unsigned int& outNS) {
    unsigned long long total = (unsigned long long)baseNS + addNS;
    outS = baseS + (unsigned int)(total / BILLION);
    outNS = (unsigned int)(total % BILLION);
}

static void addServiceTime(PCB& p, unsigned int usedNS) {
    unsigned long long total =
        (unsigned long long)p.serviceTimeNano + usedNS;

    p.serviceTimeSeconds += (unsigned int)(total / BILLION);
    p.serviceTimeNano = (unsigned int)(total % BILLION);
}

static int findFreeSlot() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (!g_table[i].occupied) return i;
    }
    return -1;
}

static void printReadyQueue() {
	queue<int> temp = readyQueue;
	string line = "OSS: Ready queue [";
	while(!temp.empty()) {
		int idx = temp.front();
		temp.pop();
		line += " p" + to_string(g_table[idx].localPid);
	}
	line += " ]\n";
	logBoth("%s", line.c_str());
}




static void printBlockedList() {
    string line = "OSS: Blocked queue [";
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].blocked) {
            line += " p" + to_string(g_table[i].localPid);
        }
    }
    line += " ]\n";
    logBoth("%s", line.c_str());
}


static void printProcessTable() {
    logBoth("\nOSS PID:%d SysClockS: %u SysClockNano: %u\n",
            getpid(), g_clk->seconds, g_clk->nanoseconds);
    logBoth("Process Table:\n");
    logBoth("Entry Occupied PID LocalPID StartS StartN ServiceS ServiceN EventWaitS EventWaitN Blocked\n");

    for (int i = 0; i < TABLE_SIZE; i++) {
        PCB& p = g_table[i];
        logBoth("%d %d %d %d %u %u %u %u %u %u %d\n",
                i,
                p.occupied ? 1 : 0,
                (int)p.pid,
                p.localPid,
                p.startSeconds,
                p.startNano,
                p.serviceTimeSeconds,
                p.serviceTimeNano,
                p.eventWaitSec,
                p.eventWaitNano,
                p.blocked ? 1 : 0);
    }
    printReadyQueue();
    printBlockedList();
    logBoth("\n");
}

static void cleanup() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].pid > 0) {
            kill(g_table[i].pid, SIGTERM);
        }
    }

    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].pid > 0) {
            waitpid(g_table[i].pid, nullptr, 0);
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
         << " [-h] [-n proc] [-s simul] [-t timelimitForChildren] "
         << "[-i fractionOfSecondToLaunchChildren] [-f logfile]\n";
}

static unsigned int randomCpuBurstNS(double maxT) {
    unsigned long long maxNS = (unsigned long long)(maxT * 1000000000.0);
    if (maxNS == 0) return 1;
    return (unsigned int)(1 + (rand() % maxNS));
}

static bool queueContains(queue<int> q, int target) {
    while (!q.empty()) {
        if (q.front() == target) return true;
        q.pop();
    }
    return false;
}

static void checkBlockedProcesses() {
    for (int i = 0; i < TABLE_SIZE; i++) {
        if (g_table[i].occupied && g_table[i].blocked) {
            if (timeGTE(g_clk->seconds, g_clk->nanoseconds,
                        g_table[i].eventWaitSec, g_table[i].eventWaitNano)) {
                g_table[i].blocked = false;
                if (!queueContains(readyQueue, i)) {
                    readyQueue.push(i);
                }
                addOverhead(10000); // waking/unblocking overhead
                logBoth("OSS: Unblocking process P%d at time %u:%u\n",
                        g_table[i].localPid, g_clk->seconds, g_clk->nanoseconds);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGALRM, signal_handler);
    alarm(3); 

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
                if (n <= 0 || n > 80) {
                    cerr << "Error: -n must be 1..80\n";
                    return 1;
                }
                break;
            case 's':
                s = atoi(optarg);
                if (s <= 0 || s > 20) {
                    cerr << "Error: -s must be 1..20\n";
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
                    cerr << "Error: -i must be nonnegative\n";
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

    unsigned int intervalNS = (unsigned int)(interval * 1000000000.0);

    int launched = 0;
    int activeChildren = 0;
    int nextLocalPid = 1;

    unsigned int nextLaunchS = 0;
    unsigned int nextLaunchNS = 0;

    unsigned int lastPrintS = 0;
    unsigned int lastPrintNS = 0;

    while (launched < n || activeChildren > 0) {
        // launch new child if time allows and capacity allows
        if (launched < n && activeChildren < s &&
            timeGTE(g_clk->seconds, g_clk->nanoseconds, nextLaunchS, nextLaunchNS)) {

            int slot = findFreeSlot();
            if (slot != -1) {
                int localPid = nextLocalPid++;
                unsigned int totalCpuBurstNs = randomCpuBurstNS(t);

                pid_t child = fork();
                if (child == 0) {
                    string pidStr = to_string(localPid);
                    string burstStr = to_string(totalCpuBurstNs);
                    execl("./worker", "worker",
                          pidStr.c_str(), burstStr.c_str(),
                          (char*)nullptr);
                    cerr << "OSS: execl failed: " << strerror(errno) << "\n";
                    _exit(1);
                } else if (child > 0) {
                    g_table[slot].occupied = true;
                    g_table[slot].pid = child;
                    g_table[slot].localPid = localPid;
                    g_table[slot].startSeconds = g_clk->seconds;
                    g_table[slot].startNano = g_clk->nanoseconds;
                    g_table[slot].serviceTimeSeconds = 0;
                    g_table[slot].serviceTimeNano = 0;
                    g_table[slot].eventWaitSec = 0;
                    g_table[slot].eventWaitNano = 0;
                    g_table[slot].blocked = false;

                    readyQueue.push(slot);

                    launched++;
                    activeChildren++;

                    addOverhead(10000); // launch overhead

                    addTimeToPair(g_clk->seconds, g_clk->nanoseconds,
                                  intervalNS, nextLaunchS, nextLaunchNS);

                    logBoth("OSS: Generating process with PID %d and putting it in ready queue at time %u:%u\n",
                            localPid, g_clk->seconds, g_clk->nanoseconds);
                } else {
                    cerr << "OSS: fork failed: " << strerror(errno) << "\n";
                }
            }
        }

        // unblock processes whose event time has arrived
        checkBlockedProcesses();

        // if a ready process exists, dispatch it
        if (!readyQueue.empty()) {
            printReadyQueue();

            int slot = readyQueue.front();
            readyQueue.pop();

            PCB& p = g_table[slot];

            if (!p.occupied || p.blocked) {
                continue;
            }

            addOverhead(1000); // scheduling decision overhead

            logBoth("OSS: Dispatching process with PID %d from ready queue at time %u:%u\n",
                    p.localPid, g_clk->seconds, g_clk->nanoseconds);
            logBoth("OSS: total time this dispatch was 1000 nanoseconds\n");

            Message msg;
            msg.mtype = p.pid; // send directly to this worker
            msg.index = slot;
            msg.quantum = QUANTUM_NS;
            msg.usedTime = 0;
            msg.action = ACTION_FULL_QUANTUM;

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

            addToClock(reply.usedTime);
            g_totalBusyTime += reply.usedTime;
            addServiceTime(p, reply.usedTime);

            logBoth("OSS: Receiving that process with PID %d ran for %u nanoseconds\n",
                    p.localPid, reply.usedTime);

            if (reply.action == ACTION_TERMINATED) {
                logBoth("OSS: Process with PID %d terminated at time %u:%u\n",
                        p.localPid, g_clk->seconds, g_clk->nanoseconds);

                int status = 0;
                waitpid(p.pid, &status, 0);
                memset(&g_table[slot], 0, sizeof(PCB));
                activeChildren--;
            }
            else if (reply.action == ACTION_BLOCKED) {
                logBoth("OSS: not using its entire time quantum\n");
                logBoth("OSS: Putting process with PID %d into blocked queue\n",
                        p.localPid);

                p.blocked = true;
                addTimeToPair(g_clk->seconds, g_clk->nanoseconds,
                              BLOCKED_TIME_NS,
                              p.eventWaitSec, p.eventWaitNano);
            }
            else {
                logBoth("OSS: Putting process with PID %d into ready queue\n",
                        p.localPid);
                readyQueue.push(slot);
            }
        }
        else {
            // no ready process; advance time until something happens
            addOverhead(10000);
        }

        // reap any unexpected dead children
        while (true) {
            int status = 0;
            pid_t dead = waitpid(-1, &status, WNOHANG);
            if (dead <= 0) break;

            for (int i = 0; i < TABLE_SIZE; i++) {
                if (g_table[i].occupied && g_table[i].pid == dead) {
                    memset(&g_table[i], 0, sizeof(PCB));
                    activeChildren--;
                    break;
                }
            }
        }

        // print every half second simulated time
        if (nanosBetween(lastPrintS, lastPrintNS,
                         g_clk->seconds, g_clk->nanoseconds) >= 500000000LL) {
            printProcessTable();
            lastPrintS = g_clk->seconds;
            lastPrintNS = g_clk->nanoseconds;
        }
    }

    unsigned long long totalTime = g_totalBusyTime + g_totalOverheadTime;
    double cpuUtil = 0.0;
    if (totalTime > 0) {
        cpuUtil = (double)g_totalBusyTime / (double)totalTime * 100.0;
    }

    logBoth("\nOSS Summary:\n");
    logBoth("Total processes launched: %d\n", launched);
    logBoth("Total worker busy time (ns): %llu\n", g_totalBusyTime);
    logBoth("Total oss overhead time (ns): %llu\n", g_totalOverheadTime);
    logBoth("Average CPU utilization: %.2f%%\n", cpuUtil);

    cleanup();
    return 0;
}
