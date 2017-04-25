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
        void processAcks();

        // Public Receiver Member Functions
        void reliableReceive(char * filename);
    private:
        // Private Sender Member Functions
        void sendWindow();
        void senderSetupConnection();
        void senderTearDownConnection();

        // Private Receiver Memeber Functions
        bool receivePacket();
        void receiverSetupConnection();
        void receiverTearDownConnection();

        // Private Startup Handshake functions
        int receiveStartSyn();
        int receiveStartSynAck();
        int receiveStartAck(int synAckSeqNum);

        // Private Teardown Handshake functions
        int receiveEndFin();
        int receiveEndFinAck();
        int receiveEndAck();

        // socket communication
        int sockfd;
        struct sockaddr receiverAddr, senderAddr;          // needed for sendto
        socklen_t receiverAddrLen, senderAddrLen;          // needed for sendto
        struct timeval rtt;

        // Circular buffer that contains packets
        CircularBuffer * buffer;

        // Book keeping
        tcp_state_t state;
};


#endif
