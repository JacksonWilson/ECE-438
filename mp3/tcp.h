#ifndef TCP_H
#define TCP_H

#include "parameters.h"
#include "types.h"
#include "circular_buffer.h"

class TCP
{
    public:
        // Sender Constructor
        TCP(char * hostname, char * hostUDPport);
        // Receiver Constructor
        TCP(char * hostUDPport);
        ~TCP();

        // Public Sender Member Functions
        void reliableSend(char * filename, unsigned long long int bytesToTransfer);
        void sendWindow();

        // Public Receiver Member Functions
        void reliableReceive(char * filename);
    private:
        // Private Sender Member Functions
        void senderSetupConnection();
        void senderTearDownConnection();

        // Private Receiver Memeber Functions
        bool receivePacket();
        void receiverSetupConnection();
        void receiverTearDownConnection();

        // Private Startup Handshake functions
        int receiveStartSyn();
        int receiveStartSynAck(struct timeval synZeroTime);
        void receiveStartAck(msg_header_t syn_ack);

        // Private Teardown Handshake functions
        int receiveEndFinAck();
        void receiveEndAck(msg_header_t fin_ack);

        // ACK Processing
        bool ackManager();
        void processTO();
        void processAcks(ack_process_t & pACK);
        unsigned long long processExpecAck(ack_process_t & pACK,  uint32_t ackReceivedIdx);
        unsigned long long processDupAck(ack_process_t & pACK);
        unsigned long long processOoOAck(ack_process_t & pACK,  uint32_t ackReceivedIdx);


        // Fast recover and fast retransmit functions
        void resendTOWindow();
        void resendWindow();
        void updateWindowSettings(ack_process_t & pACK);

        // RTT function
        void updateTimingConstraints(unsigned long long rttSample);
        double stdDevRTT();
        double stdWeight();
        double srttWeight();

        // socket communication
        int sockfd;
        struct sockaddr receiverAddr, senderAddr;          // needed for sendto
        socklen_t receiverAddrLen, senderAddrLen;          // needed for sendto

        // Circular buffer that contains packets
        CircularBuffer * buffer;

        // Round trip time and Retransmit time out
        struct timeval rto;
        double rtoNext;
        double srtt;
        deque<unsigned long long> rttHistory;               // Basically a queue that we can itterate through
        unsigned long long rttRunningTotal;
        unsigned long long numRTTTotal;
        double alpha;

        // Book keeping
        tcp_state_t state;
        send_state_t sendState;
        int expectedAckSeqNum;
        int lastPacketSent;
        int numRetransmissions;

        // degbuging
        ofstream sfile, afile, rttfile;
};


#endif
