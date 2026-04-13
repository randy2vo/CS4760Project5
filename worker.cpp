#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <ctime>
#include "shared.h"

using namespace std;

static bool reached(unsigned int s, unsigned int ns,
                    unsigned int endS, unsigned int endNS) {
    return (s > endS) || (s == endS && ns >= endNS);
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        cerr << "Usage: ./worker <index> <endSec> <endNano>\n";
        return 1;
    }

    int index = atoi(argv[1]);
    unsigned int endSec = (unsigned int)strtoul(argv[2], nullptr, 10);
    unsigned int endNano = (unsigned int)strtoul(argv[3], nullptr, 10);

    key_t shmKey = ftok(".", 'C');
    if (shmKey == -1) {
        cerr << "Worker ftok shm failed: " << strerror(errno) << "\n";
        return 1;
    }

    int shmid = shmget(shmKey, sizeof(SimClock), 0666);
    if (shmid == -1) {
        cerr << "Worker shmget failed: " << strerror(errno) << "\n";
        return 1;
    }

    SimClock* clk = (SimClock*)shmat(shmid, nullptr, 0);
    if (clk == (SimClock*)-1) {
        cerr << "Worker shmat failed: " << strerror(errno) << "\n";
        return 1;
    }

    key_t msgKey = ftok(".", 'Q');
    if (msgKey == -1) {
        cerr << "Worker ftok msg failed: " << strerror(errno) << "\n";
        shmdt(clk);
        return 1;
    }

    int msgid = msgget(msgKey, 0666);
    if (msgid == -1) {
        cerr << "Worker msgget failed: " << strerror(errno) << "\n";
        shmdt(clk);
        return 1;
    }

    srand((unsigned int)(time(nullptr) ^ getpid()));

    int owned[NUM_RESOURCES] = {0};

    while (true) {
        Message msg;
        if (msgrcv(msgid, &msg, sizeof(Message) - sizeof(long), getpid(), 0) == -1) {
            if (errno == EIDRM || errno == EINTR) {
                shmdt(clk);
                return 0;
            }
            cerr << "Worker msgrcv failed: " << strerror(errno) << "\n";
            shmdt(clk);
            return 1;
        }

        // If oss woke us up after a blocked request was granted
        if (msg.granted >= 0 && msg.granted < NUM_RESOURCES) {
            owned[msg.granted]++;
        }

        if (reached(clk->seconds, clk->nanoseconds, endSec, endNano)) {
            Message reply;
            reply.mtype = 1;
            reply.index = index;
            reply.action = 0;   // terminate
            reply.granted = -1;
            msgsnd(msgid, &reply, sizeof(Message) - sizeof(long), 0);
            break;
        }

        int percent = rand() % 100;
        Message reply;
        reply.mtype = 1;
        reply.index = index;
        reply.granted = -1;

        // Prefer requests over releases, around 70/30
        if (percent < 70) {
            int r = rand() % NUM_RESOURCES;
            if (owned[r] < INSTANCES_PER_RESOURCE) {
                reply.action = r + 1; // request resource r
            } else {
                bool released = false;
                for (int i = 0; i < NUM_RESOURCES; i++) {
                    if (owned[i] > 0) {
                        owned[i]--;
                        reply.action = -(i + 1);
                        released = true;
                        break;
                    }
                }
                if (!released) {
                    int rr = rand() % NUM_RESOURCES;
                    reply.action = rr + 1;
                }
            }
        } else {
            bool released = false;
            for (int tries = 0; tries < NUM_RESOURCES; tries++) {
                int r = rand() % NUM_RESOURCES;
                if (owned[r] > 0) {
                    owned[r]--;
                    reply.action = -(r + 1); // release resource r
                    released = true;
                    break;
                }
            }

            if (!released) {
                int r = rand() % NUM_RESOURCES;
                reply.action = r + 1;
            }
        }

        if (msgsnd(msgid, &reply, sizeof(Message) - sizeof(long), 0) == -1) {
            cerr << "Worker msgsnd failed: " << strerror(errno) << "\n";
            shmdt(clk);
            return 1;
        }
    }

    shmdt(clk);
    return 0;
}
