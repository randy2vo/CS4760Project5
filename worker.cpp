#include <iostream>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <ctime>
#include "shared.h"

using namespace std;

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage: ./worker <localPid> <totalCpuBurstNs>\n";
        return 1;
    }

    int localPid = atoi(argv[1]);
    unsigned int totalCpuBurstNs = (unsigned int)strtoul(argv[2], nullptr, 10);
    unsigned int totalCpuUsed = 0;

    key_t shmKey = ftok(".", 'C');
    if (shmKey == -1) {
        cerr << "Worker: ftok for shared memory failed: " << strerror(errno) << "\n";
        return 1;
    }

    int shmid = shmget(shmKey, sizeof(SimClock), 0666);
    if (shmid == -1) {
        cerr << "Worker: shmget failed: " << strerror(errno) << "\n";
        return 1;
    }

    SimClock* clk = (SimClock*)shmat(shmid, nullptr, 0);
    if (clk == (SimClock*)-1) {
        cerr << "Worker: shmat failed: " << strerror(errno) << "\n";
        return 1;
    }

    key_t msgKey = ftok(".", 'Q');
    if (msgKey == -1) {
        cerr << "Worker: ftok for message queue failed: " << strerror(errno) << "\n";
        shmdt(clk);
        return 1;
    }

    int msgid = msgget(msgKey, 0666);
    if (msgid == -1) {
        cerr << "Worker: msgget failed: " << strerror(errno) << "\n";
        shmdt(clk);
        return 1;
    }

    srand((unsigned int)(getpid() ^ time(nullptr)));

    cout << "WORKER PID:" << getpid()
         << " localPid:" << localPid
         << " totalCpuBurstNs:" << totalCpuBurstNs
         << " startClock: " << clk->seconds << ":" << clk->nanoseconds
         << "\n";

    while (true) {
        Message msg;

        if (msgrcv(msgid, &msg, sizeof(Message) - sizeof(long), getpid(), 0) == -1) {
            if (errno == EIDRM || errno == EINTR) {
                shmdt(clk);
                return 0;
            }

            cerr << "Worker: msgrcv failed: " << strerror(errno) << "\n";
            shmdt(clk);
            return 1;
        }

        unsigned int quantum = msg.quantum;
        unsigned int remaining = totalCpuBurstNs - totalCpuUsed;

        Message reply;
        reply.mtype = 1;          // send back to oss
        reply.index = msg.index;
        reply.quantum = quantum;
        reply.usedTime = 0;
        reply.action = ACTION_FULL_QUANTUM;

        // Case 1: process finishes during this dispatch
        if (remaining <= quantum) {
            reply.usedTime = remaining;
            reply.action = ACTION_TERMINATED;
            totalCpuUsed += remaining;

            cout << "WORKER PID:" << getpid()
                 << " terminating after using "
                 << reply.usedTime << " ns"
                 << " totalUsed=" << totalCpuUsed << "\n";

            if (msgsnd(msgid, &reply, sizeof(Message) - sizeof(long), 0) == -1) {
                cerr << "Worker: msgsnd failed: " << strerror(errno) << "\n";
                shmdt(clk);
                return 1;
            }

            break;
        }

        // 20% chance to block
        int chance = rand() % 100;
        if (chance < 20) {
            reply.usedTime = 1 + (rand() % quantum);   // partial quantum
            reply.action = ACTION_BLOCKED;
            totalCpuUsed += reply.usedTime;

            cout << "WORKER PID:" << getpid()
                 << " blocked after using "
                 << reply.usedTime << " ns"
                 << " totalUsed=" << totalCpuUsed << "\n";
        } else {
            // use full quantum
            reply.usedTime = quantum;
            reply.action = ACTION_FULL_QUANTUM;
            totalCpuUsed += quantum;

            cout << "WORKER PID:" << getpid()
                 << " used full quantum "
                 << reply.usedTime << " ns"
                 << " totalUsed=" << totalCpuUsed << "\n";
        }

        if (msgsnd(msgid, &reply, sizeof(Message) - sizeof(long), 0) == -1) {
            cerr << "Worker: msgsnd failed: " << strerror(errno) << "\n";
            shmdt(clk);
            return 1;
        }
    }

    shmdt(clk);
    return 0;
}
