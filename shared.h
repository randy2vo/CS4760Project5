#ifndef SHARED_H
#define SHARED_H

#include <sys/types.h>

// ------------------------------
// Constants
// ------------------------------
const int TABLE_SIZE = 20;
const unsigned int BILLION = 1000000000U;
const unsigned int QUANTUM_NS = 25000000U;   // 25 ms
const unsigned int BLOCKED_TIME_NS = 100000000U; // 100 ms

// Worker actions returned to oss
const int ACTION_FULL_QUANTUM = 0;
const int ACTION_BLOCKED      = 1;
const int ACTION_TERMINATED   = 2;

// ------------------------------
// Shared simulated clock
// ------------------------------
struct SimClock {
    unsigned int seconds;
    unsigned int nanoseconds;
};

// ------------------------------
// Message queue structure
// mtype = destination process type
// ------------------------------
struct Message {
    long mtype;                // required by System V queues
    int index;                 // PCB slot or simulated pid
    unsigned int quantum;      // quantum sent by oss
    unsigned int usedTime;     // time actually used by worker
    int action;                // full quantum / blocked / terminated
};

// ------------------------------
// Process Control Block
// ------------------------------
struct PCB {
    bool occupied;                   // slot in use or not
    pid_t pid;                       // real Linux pid
    int localPid;                    // simulated pid for logging

    unsigned int startSeconds;       // when created
    unsigned int startNano;

    unsigned int serviceTimeSeconds; // total CPU time used
    unsigned int serviceTimeNano;

    unsigned int eventWaitSec;       // when blocked process wakes up
    unsigned int eventWaitNano;

    bool blocked;                    // currently blocked?
};

#endif
