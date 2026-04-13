#ifndef SHARED_H
#define SHARED_H

#include <sys/types.h>

const int TABLE_SIZE = 20;
const int MAX_ACTIVE_PROCS = 18;
const int NUM_RESOURCES = 10;
const int INSTANCES_PER_RESOURCE = 5;
const unsigned int BILLION = 1000000000U;
const unsigned int CLOCK_INCREMENT_NS = 10000000U; // 10 ms

struct SimClock {
    unsigned int seconds;
    unsigned int nanoseconds;
};

struct PCB {
    int occupied;
    pid_t pid;
    int localPid;

    unsigned int startSeconds;
    unsigned int startNano;

    unsigned int endSeconds;
    unsigned int endNano;

    int blocked;                    // 1 if blocked, 0 otherwise
    int requestedResource;          // 0..9 if blocked, -1 otherwise
    int resourcesAllocated[NUM_RESOURCES];
};

struct Message {
    long mtype;     // child pid when oss->worker, 1 when worker->oss
    int index;      // PCB slot
    int action;     // >0 request R(action-1), <0 release R((-action)-1), 0 terminate
    int granted;    // oss can set this when waking a blocked process
};

#endif
