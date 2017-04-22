#ifndef TYPES_H
#define TYPES_H

#include <cstdlib>
#include <stdio.h>
#include <iostream>
#include <string>
#include <limits>
#include <vector>
#include <queue>
#include <stack>
#include <utility>
#include <string>
#include <mutex>
#include <thread>
#include <chrono>
#include <functional>
#include <atomic>
#include "ls_router.h"

using std::thread;
using std::ref;

using std::cout;
using std::endl;
using std::vector;
using std::numeric_limits;
using std::priority_queue;
using std::pair;
using std::make_pair;
using std::greater;
using std::queue;
using std::stack;
using std::string;
using std::to_string;
using std::mutex;

typedef pair<int,int> int_pair;


typedef struct {
    unsigned char seqNum;
} msg_header_t;

typedef struct {
    msg_header_t header;
    unsigned char msg[PAYLOAD - sizeof(msg_header_t)];
} msg_packet_t;

typedef struct {
    
} ack_packet_t;

#endif
