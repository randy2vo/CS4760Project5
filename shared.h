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

    int blocked;                    
    int requestedResource;          
    int resourcesAllocated[NUM_RESOURCES];
    int pendingGrants;
};

struct Message {
    long mtype;     
    int index;      
    int action;     
    int granted;    
};

#endif
