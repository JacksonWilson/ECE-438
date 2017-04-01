
#ifndef LS_ROUTER_H
#define LS_ROUTER_H

#include "router.h"
#include "graph.h"

#define TRUE 1
#define FALSE 0

typedef struct
{
    int sourceNode;
    int destNode;
    int cost;
    int valid;
} Link_t;

typedef struct
{
    char header[4] = "lsp";
    int producerNode;
    int sequence_num;
    // struct timespec ttl;
    Link_t updatedLink;
} LSPL_t;

class LS_Router : public Router
{
    public:
        // Constructor and Destructor
        LS_Router(int id, char * graphFileName, char * logFileName);

        // Member Functions
        void listenForNeighbors();
        void updateForwardingTable();
        void checkHeartBeat();
        void periodicLSPL();
        void generateLSPL(int sourceNode, int destNode);
        void forwardLSPL(char * LSPL_Buf, int heardFromNode);
        void sendLSPU(int destNode);

    private:
        // Functions to convert to an from network order
        LSPL_t hostToNetworkLSPL(LSPL_t * hostval);
        LSPL_t networkToHostLSPL(LSPL_t * networkval);

        // Graph stores the current network topology
        Graph network;

        // sequence numbers for LSP packets
        vector<int> seqNums;
};


#endif
