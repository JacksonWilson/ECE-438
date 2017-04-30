
#include "tcp.h"

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa) {
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

void bufferFiller(CircularBuffer & buffer) {
	buffer.fillBuffer();
}

void packetSender(TCP & connection) {
	connection.sendWindow();
}

TCP::~TCP(){
	delete buffer;
	close(sockfd);
}

/*************** Sender Functions ***************/
TCP::TCP(char * hostname, char * hostUDPport)
{
	struct addrinfo hints, *servinfo, *p;
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;

	if ((rv = getaddrinfo(hostname, hostUDPport, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		exit(1);
	}

	if (servinfo == NULL) {
		fprintf(stderr, "talker: failed to resolve addr\n");
		exit(2);
	}

	receiverAddr  = *(servinfo->ai_addr);
	receiverAddrLen = servinfo->ai_addrlen;

	freeaddrinfo(servinfo);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;

	if ((rv = getaddrinfo(NULL, hostUDPport, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		exit(1);
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("listener: socket");
			continue;
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("listener: bind");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "listener: failed to bind socket\n");
		exit(2);
	}

	freeaddrinfo(servinfo);

	// Initial time out estimation
	rto.tv_sec = 0;
	rto.tv_usec = INIT_RTO;
	if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto)) < 0) {
		perror("setsockopt");
		exit(3);
	}

	// Book keeping
	expectedAckSeqNum = 0;
	lastPacketSent = -1;
	numRetransmissions = 0;
	srtt = 0.0;
	rttRunningTotal = 0;
	numRTTTotal = 0;

	state = CLOSED;
	sendState = WAITING_TO_SEND;
	alpha = ALPHA;

	#ifdef DEBUG
		sfile.open("sendLog", std::ios::out);
		afile.open("ackLog", std::ios::out);
		rttfile.open("rttLog", std::ios::out);
	#endif

}

void TCP::senderSetupConnection()
{
	struct timeval synTime;
	msg_header_t syn;
	syn.type = SYN_HEADER;
	syn.seqNum = htonl(0);

	state = LISTEN;

	// send SYN
	gettimeofday(&synTime, 0);
	sendto(sockfd, (char *)&syn, sizeof(msg_header_t), 0, &receiverAddr, receiverAddrLen);

	state = SYN_SENT;

	// wait for SYN + ACK
	ack_packet_t ack;
	ack.type = ACK_HEADER;
	ack.seqNum = receiveStartSynAck(synTime);

	// send ACK
	sendto(sockfd, (char *)&ack, sizeof(ack_packet_t), 0, &receiverAddr, receiverAddrLen);

	state = ESTABLISHED;
	sendState = SLOW_START;
}

void TCP::reliableSend(char * filename, unsigned long long int bytesToTransfer)
{
    buffer = new CircularBuffer(BUFFER_SIZE, filename, bytesToTransfer);

	// Set up TCP connection
	senderSetupConnection();

	// send data
	thread bufferHandler(bufferFiller, ref(*(this->buffer))); bufferHandler.detach();
	thread sendHandler(packetSender, ref(*(this))); sendHandler.detach();
	ackManager();

	// tear down TCP connection
	senderTearDownConnection();
}

void TCP::senderTearDownConnection()
{
	msg_header_t fin;
	fin.type = FIN_HEADER;
	fin.seqNum = htonl(0);

	state = CLOSING;

	// send FIN
	sendto(sockfd, (char *)&fin, sizeof(msg_header_t), 0, &receiverAddr, receiverAddrLen);

	state = FIN_SENT;

	// wait for FIN + ACK
	ack_packet_t ack;
	ack.type = ACK_HEADER;
	ack.seqNum = receiveEndFinAck();

	// send ACK
	sendto(sockfd, (char *)&ack, sizeof(ack_packet_t), 0, &receiverAddr, receiverAddrLen);

	state = CLOSED;
}


void TCP::ackManager()
{
	ack_process_t pACK;
	struct sockaddr_storage theirAddr;
	socklen_t theirAddrLen = sizeof(theirAddr);

	while(true){
		// Transmission completed
		if(expectedAckSeqNum >= buffer->seqNum && buffer->fileLoadCompleted == true){
			return;
		}

		// Wait for ack
		setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
		if(recvfrom(sockfd, (char *)&pACK.ack, sizeof(ack_packet_t), 0, (struct sockaddr*)&theirAddr, &theirAddrLen) == -1){
			processTO();
			continue;
		}

		// drop buffered syn_ack messages
		if(pACK.ack.type != ACK_HEADER) continue;

		//process ack if received
		processAcks(pACK);

		// decrease the effect of numRetransmissions as quickly as possible ... but not too quickly
		numRetransmissions = numRetransmissions/2;
	}
}

void TCP::processTO()
{
	#ifdef DEBUG
		// afile << "\n\nTIME OUT OCCURED: " << US_PER_SEC*rto.tv_sec + rto.tv_usec << "\n\n"; afile << "NUMBER RETRANSMIT: " << numRetransmissions + 1 << endl; afile << "EXPECTED: " << expectedAckSeqNum << endl; afile << "\n\nWINDOW SIZE: " << buffer->windowSize << "\n\n"; afile.flush();
	#endif

	// Send Window Settings
	sendState = AIMD;
	buffer->windowSize = max((buffer->windowSize)/2, (uint32_t)MIN_WINDOW_SIZE);

	// recalculate timing constraints
	numRetransmissions++;
	rttHistory.erase(rttHistory.begin(), rttHistory.begin() + min((size_t)(numRetransmissions*DROP_HIST_WEIGHT), rttHistory.size() - 1));

	// Update RT
	rtoNext = min(1.5*rtoNext, (double)MAX_RTO);
	rto.tv_sec = ((unsigned long long)rtoNext)/(US_PER_SEC);
	rto.tv_usec = ((unsigned long long)rtoNext)%(US_PER_SEC);

	// Resend window
	if(resendTOWindow() == false) return;
}


void TCP::processAcks(ack_process_t & pACK)
{
	unsigned long long rttSample;

	gettimeofday(&(pACK.time), 0);
	pACK.ack.seqNum = ntohl(pACK.ack.seqNum);
	uint32_t ackReceivedIdx = (pACK.ack.seqNum % buffer->data.size());

	#ifdef DEBUG
		static auto last = std::chrono::high_resolution_clock::now(); auto current = std::chrono::high_resolution_clock::now(); afile << "TIME diff trans: " << std::chrono::duration<double, std::milli>(current - last).count() << " milliseconds.\n"; afile.flush(); last = current; afile << "expected: " << expectedAckSeqNum << ", saw: " << pACK.ack.seqNum << ", TIME: " << buffer->timeSinceStart() << "us" << endl; afile.flush();
	 #endif

	if(expectedAckSeqNum == pACK.ack.seqNum){
		rttSample = processExpecAck(pACK, ackReceivedIdx);
		updateTimingConstraints(rttSample);
	} else if(expectedAckSeqNum < pACK.ack.seqNum){
		rttSample = processOoOAck(pACK, ackReceivedIdx);
	} else if(expectedAckSeqNum == pACK.ack.seqNum + 1){
		rttSample = processDupAck(pACK);
	}
}

unsigned long long TCP::processExpecAck(ack_process_t & pACK,  uint32_t ackReceivedIdx)
{
	unsigned long long rttSample;

	updateWindowSettings(pACK);

	{
		unique_lock<mutex> lkAck(buffer->idxLock);
		buffer->state[ackReceivedIdx] = AVAILABLE;

		rttSample = US_PER_SEC*(pACK.time.tv_sec - buffer->timestamp[ackReceivedIdx].tv_sec) + pACK.time.tv_usec - buffer->timestamp[ackReceivedIdx].tv_usec;

		#ifdef DEBUG
			afile << "ACK in-order: " << ntohl(buffer->data[ackReceivedIdx].header.seqNum) << ", TIME: " << buffer->timeSinceStart() << "us" << endl;
			afile.flush();
		#endif

		lkAck.unlock();
		buffer->openWinCV.notify_all();
	}

	return rttSample;
}

unsigned long long TCP::processOoOAck(ack_process_t & pACK,  uint32_t ackReceivedIdx)
{
	unsigned long long rttSample;

	int curExpectedAckSeqNum = expectedAckSeqNum;

	updateWindowSettings(pACK);

	// Handling missing acks based on cumulative out of order ACK
	for(unsigned int i = (curExpectedAckSeqNum % buffer->data.size()); i != (ackReceivedIdx); i = ((i + 1) % buffer->data.size())){
		unique_lock<mutex> lkOoO(buffer->idxLock);

		#ifdef DEBUG
		afile << "ACK Out of order process: " << ntohl(buffer->data[i].header.seqNum) << ", TIME: " << buffer->timeSinceStart() << "us" << endl;
		afile.flush();
		#endif

		if(buffer->state[i] == SENT){
			buffer->state[i] = AVAILABLE;
		}

		lkOoO.unlock();
		buffer->openWinCV.notify_all();
	}

	{  // handling acked message
		unique_lock<mutex> lkAck(buffer->idxLock);
		buffer->state[ackReceivedIdx] = AVAILABLE;

		#ifdef DEBUG
		afile << "ACK Out of order process (received): " << pACK.ack.seqNum << ", TIME: " << buffer->timeSinceStart() << "us" << endl;
		afile.flush();
		#endif

		rttSample = US_PER_SEC*(pACK.time.tv_sec - buffer->timestamp[ackReceivedIdx].tv_sec) + pACK.time.tv_usec - buffer->timestamp[ackReceivedIdx].tv_usec;
		lkAck.unlock();
		buffer->openWinCV.notify_all();
	}

	return rttSample;
}

unsigned long long TCP::processDupAck(ack_process_t & pACK)
{
	#ifdef DEBUG
		afile << "\n\nDUPLICATE ACK: " << pACK.ack.seqNum << ", TIME: " << buffer->timeSinceStart() << "us" << "\n\n";
		afile.flush();
	#endif

	static int dupAckLastSeen = -1;
	static uint8_t counter  = 0;
	static uint8_t counterPost = 0;

	if(dupAckLastSeen == pACK.ack.seqNum){
		counter++;
		if(counter == DUP_MAX_COUNTER){
			counterPost = 0;
			counterPost++;
			resendWindow();
		}else if(counterPost >= ((buffer->windowSize)/3)){
			counterPost = 0;
			resendWindow();
		}else{
			counterPost++;
		}
	}else{
		counter = 0;
		counterPost = 0;
		dupAckLastSeen = pACK.ack.seqNum;
	}

	return 0;
}

void TCP::updateWindowSettings(ack_process_t & pACK)
{
	unique_lock<mutex> lkWin(buffer->idxLock);

	if((sendState == SLOW_START) || (sendState == AIMD && (pACK.ack.seqNum % buffer->windowSize) == (buffer->windowSize - 1))){
		buffer->windowSize = min((buffer->windowSize + 1), (uint32_t) MAX_WINDOW_SIZE);
		// cout << "WINDOW SIZE: " << buffer->windowSizsube << "\n";
	}

	buffer->sIdx = (pACK.ack.seqNum + 1)% BUFFER_SIZE;
	buffer->eIdx = (pACK.ack.seqNum + (buffer->windowSize))% BUFFER_SIZE;
	expectedAckSeqNum = pACK.ack.seqNum + 1;

	lkWin.unlock();
	buffer->openWinCV.notify_all();
}

bool TCP::resendTOWindow()
{
	struct sockaddr_storage theirAddr;
	socklen_t theirAddrLen = sizeof(theirAddr);
	ack_process_t pACK;

	// set timing options for acks during retransmission
	struct timeval retransCheckTime;
	retransCheckTime.tv_sec = 0;
	retransCheckTime.tv_usec = RETRANS_CHECK_TIME;
	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &retransCheckTime, sizeof(retransCheckTime));

	int j = buffer->sIdx;
	for(unsigned int i = 0; i < buffer->data.size(); i++) {
		unique_lock<mutex> lkTO(buffer->pktLocks[j]);
		if(buffer->state[j] == SENT){
			#ifdef DEBUG
			afile << "Retransmit: " <<  ntohl(buffer->data[j].header.seqNum) << ", TIME: " << buffer->timeSinceStart() << "us" << endl;
			afile.flush();
			#endif

			// TODO: Decide what to do what RTT for retransmitted packet
			gettimeofday(&(buffer->timestamp[j]), 0);
			sendto(sockfd, (char *)&(buffer->data[j]), buffer->length[j], 0, &receiverAddr, receiverAddrLen);

			if(recvfrom(sockfd, (char *)&pACK.ack, sizeof(ack_packet_t), 0, (struct sockaddr*)&theirAddr, &theirAddrLen) != -1){
				lkTO.unlock();
				processAcks(pACK);
				return false;
			}
		}
		j = (j + 1) % BUFFER_SIZE;
	}

	return true;
}

void TCP::resendWindow()
{
	#ifdef DEBUG
	afile << "RESENDING WINDOW TITLE:\n"; afile.flush();
	#endif

	int j = buffer->sIdx;
	for(unsigned int i = 0; i < (buffer->windowSize)/2; i++) {
		unique_lock<mutex> lkTO(buffer->pktLocks[j]);
		if(buffer->state[j] == SENT){
			#ifdef DEBUG
			afile << "Retransmit (DUPLICATE ACK): " <<  ntohl(buffer->data[j].header.seqNum) << ", TIME: " << buffer->timeSinceStart() << "us" << endl; afile.flush();
			#endif
			gettimeofday(&(buffer->timestamp[j]), 0);
			sendto(sockfd, (char *)&(buffer->data[j]), buffer->length[j], 0, &receiverAddr, receiverAddrLen);
		}
		j = (j + 1) % BUFFER_SIZE;
	}
}

void TCP::updateTimingConstraints(unsigned long long rttSample)
{
	if(rttHistory.size() >= MAX_RTT_HISTORY){
		// once we have hit the max history, start dorping values
		rttHistory.pop_front();
		rttHistory.push_back(rttSample);
	}else{
		// build up history
		rttHistory.push_back(rttSample);
	}

	// update running totals
	rttRunningTotal += rttSample;
	numRTTTotal++;

	// update SRTT
	alpha = min(ALPHA_TO_SCALAR*numRetransmissions + ALPHA, ALPHA_MAX);
	srtt = (1.0 - alpha)*((double)srtt) + alpha*((double)rttSample);

	// update RTO
	rtoNext = srttWeight()*srtt + stdWeight()*stdDevRTT();
	rtoNext = min(rtoNext, (double)MAX_RTO);
	rto.tv_sec = ((unsigned long long)rtoNext)/(US_PER_SEC);
	rto.tv_usec = ((unsigned long long)rtoNext)%(US_PER_SEC);

	#ifdef DEBUG
		// rttfile << "SRTT weight: " << srttWeight() << endl;
		// rttfile << "STD weight: " << stdWeight()  << endl << endl;
		// rttfile << "History Size: " << rttHistory.size() << endl;
		rttfile << "Expected: " << expectedAckSeqNum << ", TIME: " << buffer->timeSinceStart() << "us" << endl;
		rttfile << "StdDev: " << stdDevRTT() << endl;
		rttfile << "RTT in microseconds: " << rttHistory.back() << endl;
		rttfile << "RTO in microseconds: " << rtoNext << "\n\n";
	#endif
}

double TCP::stdDevRTT()
{
	double meanRTT = (1.0*(double)rttRunningTotal)/((double)numRTTTotal);

	double sqrdMeanDiff = 0.0;
	for(unsigned long long rttSample : rttHistory) {
		sqrdMeanDiff += ((rttSample - meanRTT)*(rttSample - meanRTT));
    }
	sqrdMeanDiff = sqrdMeanDiff/(rttHistory.size());

	return sqrt(sqrdMeanDiff);
}

inline double TCP::stdWeight(){
	return (STD_SLOPE*((double)rttHistory.size()));
}

inline double TCP::srttWeight(){
	return (SRTT_SLOPE*((double)rttHistory.size()) + MAX_SRTT_WEIGHT);
}

void TCP::sendWindow()
{
	#ifdef DEBUG
		auto last = std::chrono::high_resolution_clock::now();
	#endif

	while(state == ESTABLISHED){
		unique_lock<mutex> lkWin(buffer->idxLock);
		buffer->openWinCV.wait(lkWin, [=]{
			return ((uint32_t)(lastPacketSent - expectedAckSeqNum) != (buffer->windowSize - 1));
		});
		uint32_t eIdx = buffer->eIdx;
		lkWin.unlock();

		uint32_t i = (lastPacketSent + 1) % BUFFER_SIZE; // protect with locks

		for(;i != eIdx; i = (i + 1)%BUFFER_SIZE) {
 			if(buffer->state[i] == FILLED){
				#ifdef DEBUG
					const auto current = std::chrono::high_resolution_clock::now(); sfile << ntohl(buffer->data[i].header.seqNum); sfile << " - TIME diff trans: " << std::chrono::duration<double, std::milli>(current - last).count() << " milliseconds.\n"; sfile.flush(); last = current;
				#endif

				gettimeofday(&(buffer->timestamp[i]), 0);
				sendto(sockfd, (char *)&(buffer->data[i]), buffer->length[i], 0, &receiverAddr, receiverAddrLen);
				buffer->state[i] = SENT;

				// book keeping
				lastPacketSent++;
			}
		}

		{	// handlign edge case of idx == eIdx
			if(buffer->state[i] == FILLED){
				#ifdef DEBUG
					const auto current = std::chrono::high_resolution_clock::now(); sfile << ntohl(buffer->data[i].header.seqNum); sfile << " - TIME diff trans: " << std::chrono::duration<double, std::milli>(current - last).count() << " milliseconds.\n"; sfile.flush(); last = current;
				#endif

				gettimeofday(&(buffer->timestamp[i]), 0);
				sendto(sockfd, (char *)&(buffer->data[i]), buffer->length[i], 0, &receiverAddr, receiverAddrLen);
				buffer->state[i] = SENT;

				// book keeping
				lastPacketSent++;
			}
		}
    }
}

/*************** Receiver Functions ***************/
TCP::TCP(char * hostUDPport)
{
	struct addrinfo hints, *servinfo, *p;
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_PASSIVE;

	if ((rv = getaddrinfo(NULL, hostUDPport, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		exit(1);
	}

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			perror("listener: socket");
			continue;
		}

		if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
			perror("listener: bind");
			continue;
		}

		break;
	}

	if (p == NULL) {
		fprintf(stderr, "listener: failed to bind socket\n");
		exit(2);
	}

	freeaddrinfo(servinfo);

	state = CLOSED;
}

void TCP::receiverSetupConnection()
{
	msg_header_t syn_ack;

	// receive SYN
	syn_ack.seqNum = receiveStartSyn();
	state = SYN_RECVD;

	// send SYN + ACK
	syn_ack.type = SYN_ACK_HEADER;
 	sendto(sockfd, (char *)&syn_ack, sizeof(msg_header_t), 0, (struct sockaddr *)&senderAddr, senderAddrLen);

	// receive ACK
	receiveStartAck(syn_ack);
}

void TCP::reliableReceive(char * filename)
{

	buffer = new CircularBuffer(BUFFER_SIZE, filename);

	state = LISTEN;

	// Set up TCP connection
	receiverSetupConnection();

	state = ESTABLISHED;

	while(true){
		if(receivePacket() == false) break;
	}

	state = CLOSING;

	// tear down TCP connection
	receiverTearDownConnection();
}

void TCP::receiverTearDownConnection()
{
	msg_header_t fin_ack;

	// FIN received in receivePacket function
	state = SYN_RECVD;

	// send FIN + ACK
	fin_ack.type = FIN_ACK_HEADER;
 	sendto(sockfd, (char *)&fin_ack, sizeof(msg_header_t), 0, (struct sockaddr *)&senderAddr, senderAddrLen);

	// receive ACK
	receiveEndAck(fin_ack);

	state = CLOSED;
}


bool TCP::receivePacket()
{
	int numbytes;
	struct sockaddr_storage their_addr;
	socklen_t addr_len;
	msg_packet_t packet;
	addr_len = sizeof(their_addr);

	if ((numbytes = recvfrom(sockfd, (char *)&packet, sizeof(msg_packet_t) , 0, (struct sockaddr *)&their_addr, &addr_len)) == -1) {
		perror("recvfrom");
		exit(1);
	}

	// start closing connection
	if(packet.header.type == FIN_HEADER) return false;

	// if garbage packet, then drop but wait to close connection
	if(packet.header.type != DATA_HEADER) return true;

	buffer->storeReceivedPacket(packet, numbytes);

	return true;
}


/*************** Startup Handshake Functions ***************/
int TCP::receiveStartSyn()
{
	struct sockaddr theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
	msg_header_t syn;
	int numbytes;

	while(true){
		if((numbytes = recvfrom(sockfd, (char *)&syn, sizeof(msg_header_t), 0, (struct sockaddr*)&theirAddr, &theirAddrLen)) == -1){
			perror("recvfrom");
		}

		if((syn.type == SYN_HEADER) && (numbytes == sizeof(msg_header_t))){
			break;
		}
	}

	senderAddr = theirAddr;
	senderAddrLen = theirAddrLen;
	buffer->setSocketAddrInfo(sockfd, senderAddr, senderAddrLen);

	return syn.seqNum;
}

int TCP::receiveStartSynAck(struct timeval synZeroTime)
{
	struct sockaddr theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
	msg_header_t syn, syn_ack;

	// Determinining initial RTT
	struct timeval synAckTime;
	vector<struct timeval> synTimeVec(START_TIME_VEC_SIZE);
	synTimeVec[0] = synZeroTime;
	unsigned long long initialRTT, initialRTO;

	syn.type = SYN_HEADER;
	int seqNum = 1;

	while(true){
		if((recvfrom(sockfd, (char *)&syn_ack, sizeof(msg_header_t), 0, (struct sockaddr*)&theirAddr, &theirAddrLen) == -1)
			|| (syn_ack.type != SYN_ACK_HEADER)){

			// store the next syntime
			syn.seqNum = htonl(seqNum);
			gettimeofday(&synTimeVec[seqNum%START_TIME_VEC_SIZE], 0);
			sendto(sockfd, (char *)&syn, sizeof(msg_header_t), 0, &receiverAddr, receiverAddrLen);
			seqNum++;
		} else{
			// Determine initial RTT
			gettimeofday(&synAckTime, 0);
			int synTimeIndex = ntohl(syn_ack.seqNum)%START_TIME_VEC_SIZE;
			initialRTT = US_PER_SEC*(synAckTime.tv_sec - synTimeVec[synTimeIndex].tv_sec) +  synAckTime.tv_usec - synTimeVec[synTimeIndex].tv_usec;

			// Assign RTO and same initialRTT
			rttRunningTotal += initialRTT;
			numRTTTotal++;
			srtt = initialRTT;
			rttHistory.push_back(initialRTT);
			initialRTO = min(2*initialRTT, (unsigned long long)INIT_RTO);
			rto.tv_sec = initialRTO/US_PER_SEC;
			rto.tv_usec = initialRTO%US_PER_SEC;

			#ifdef DEBUG
				rttfile << "SeqNum: " <<  ntohl(syn_ack.seqNum) << endl;
				rttfile << "Initial RTT in microseconds: " << rttHistory[0] << endl;
				rttfile << "Initial RTO secs: " << rto.tv_sec << endl;
				rttfile << "Initial RTO microseconds: " << rto.tv_usec << endl;
			#endif

			return syn_ack.seqNum;
		}
	}

}

void TCP::receiveStartAck(msg_header_t syn_ack)
{
	struct sockaddr theirAddr;
	socklen_t theirAddrLen = sizeof(theirAddr);
	msg_packet_t packet;
	int numbytes;

	while(true){
		if ((numbytes = recvfrom(sockfd, (char *)&packet, sizeof(msg_packet_t) , 0, (struct sockaddr *)&theirAddr, &theirAddrLen)) == -1) {
			perror("recvfrom");
		}

		// write message into buffer if ACK lost and message seen first
		if(packet.header.type == DATA_HEADER && numbytes > (int)sizeof(msg_header_t)){
			buffer->storeReceivedPacket(packet, numbytes);
			break;
		} else if(packet.header.type == ACK_HEADER){
			break;
		} else{
			sendto(sockfd, (char *)&syn_ack, sizeof(msg_header_t), 0, &theirAddr, theirAddrLen);
		}
	}
}

/*************** Teardown Handshake Functions ***************/
int TCP::receiveEndFinAck()
{
	struct sockaddr theirAddr;
    socklen_t theirAddrLen = sizeof(theirAddr);
	msg_header_t fin, fin_ack;

	fin.type = FIN_HEADER;
	int seqNum = 1;

	while(true){
		if((recvfrom(sockfd, (char *)&fin_ack, sizeof(msg_header_t), 0, (struct sockaddr*)&theirAddr, &theirAddrLen) == -1)
			|| (fin_ack.type != FIN_ACK_HEADER)){
			fin.seqNum = htonl(seqNum++);
			sendto(sockfd, (char *)&fin, sizeof(msg_header_t), 0, &receiverAddr, receiverAddrLen);
		} else{
			break;
		}
	}

	return fin_ack.seqNum;
}

void TCP::receiveEndAck(msg_header_t fin_ack)
{
	struct sockaddr theirAddr;
	socklen_t theirAddrLen = sizeof(theirAddr);
	ack_packet_t ack;

	rto.tv_sec = 0;
	rto.tv_usec = FIN_TO;
	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));

	state = TIME_WAIT;

	while(true){
		if (((recvfrom(sockfd, (char *)&ack, sizeof(ack_packet_t) , 0, (struct sockaddr *)&theirAddr, &theirAddrLen)) == -1)
			|| ack.type != FIN_HEADER){
			break;
		}else{
			// If fin_ack, lost then resend
			sendto(sockfd, (char *)&fin_ack, sizeof(msg_header_t), 0, &theirAddr, theirAddrLen);
		}
	}
}
