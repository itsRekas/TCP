#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"
#define MAX_BUFFER_SIZE 65536

/* To handle packets in a receiver window larger than one, we define a structure called packet_data. 
Each entry in this structure stores a packet’s data, its size,
and a flag indicating whether it has been received. */

typedef struct {
    char data[DATA_SIZE];
    int size;
    int received;
} packet_data;

// global pointer to send and receive packets
tcp_packet *recvpkt;
tcp_packet *sndpkt;

// Receiver window buffer that holds packets until they can be written to a file
packet_data packet_buffer[MAX_BUFFER_SIZE];

int main(int argc, char **argv) {
     /*
     * First, we define important variables:
     * - sockfd: The socket file descriptor used for receiving packets.
     * - portno: The port number to listen for incoming data.
     * - clientlen: The length of the client's address structure.
     * - serveraddr & clientaddr: Structures storing server and client details.
     * - optval: An option variable used to prevent "address already in use" errors.
     * - fp: A file pointer to store received data.
     * - buffer: A temporary buffer for incoming packets.
     * - tp: A struct used to track timestamps of received packets.
     * - next_expected_seqno: Keeps track of the next expected sequence number.
     * - last_ack_sent: Stores the last acknowledgment number sent to the sender.
     */
    int sockfd; 
    int portno; 
    int clientlen; 
    struct sockaddr_in serveraddr; 
    struct sockaddr_in clientaddr; 
    int optval; 
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;
    int next_expected_seqno = 0; 
    int last_ack_sent = 0;   

    /*
     * We check if the user has provided the correct number of arguments.
     * The program expects:
     *   - argv[1]: The port number to listen on.
     *   - argv[2]: The filename where received data will be stored.
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    /*
     * Attempt to open the file where received data will be written.
     * If the file cannot be opened, print an error and exit.
     */
    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }
     /*
     * Initialize the packet buffer to keep track of received packets.
     * Each entry starts as "not received" (received = 0) and size 0.
     */
    for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
        packet_buffer[i].received = 0;
        packet_buffer[i].size = 0;
    }

    /* 
     * socket: create the parent socket for receiving data
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * Configure the server's address and bind the socket to it.
     * This allows the server to receive data from clients.
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    
    /*
     * Main loop: Keep receiving packets and process them accordingly.
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);
    while (1) {
         /*
         * Wait for incoming data from the sender.
         * The received packet is stored in the buffer.
         */
        VLOG(DEBUG, "waiting from server \n");
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        /*
         * Cast the received buffer into a tcp_packet structure.
         * Ensure that the data size is within acceptable limits.
         */
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        /*
         * Check if we have reached the end of the file (EOF packet).
         * If so, acknowledge it and terminate the connection.
         */
        if (recvpkt->hdr.data_size == 0) { // **EOF detected**
            VLOG(INFO, "EOF reached. Sending final ACK %d", next_expected_seqno);
        
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = next_expected_seqno; // **ACK EOF**
            sndpkt->hdr.ctr_flags = ACK;
        
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            free(sndpkt);
            fclose(fp); // **Close file after EOF to ensure data is written**
            break; // **Terminate properly**
        }

         /* 
         * Once a packet is received, we extract its sequence number and data size. 
         * This helps us determine where it fits in the receiver’s window and whether 
         * we need to buffer it or process it immediately.
         */
        int packet_seqno = recvpkt->hdr.seqno;
        int packet_size = recvpkt->hdr.data_size;
        
         /* 
         * Log the received packet details, including timestamp, size, and sequence number. 
         * This helps with debugging and tracking packet flow.
         */
        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, packet_size, packet_seqno);


         // Calculate the expected sequence number and buffer position
        int buffer_pos = packet_seqno % MAX_BUFFER_SIZE;

         /* 
         * If the packet at this buffer position hasn’t been received before, we store it.
         * This ensures that out-of-order packets are kept for later reordering.
         */
        if (!packet_buffer[buffer_pos].received) {
            memcpy(packet_buffer[buffer_pos].data, recvpkt->data, packet_size);
            packet_buffer[buffer_pos].size = packet_size;
            packet_buffer[buffer_pos].received = 1;
            
            VLOG(DEBUG, "Received packet with seqno: %d, expected: %d", 
                packet_seqno, next_expected_seqno);
        }

         /* 
         * Check the buffer for packets that are now in order. 
         * If the next expected packet is available, write it to the file.
         */
        while (packet_buffer[next_expected_seqno % MAX_BUFFER_SIZE].received) {
            int pos = next_expected_seqno % MAX_BUFFER_SIZE;
            fwrite(packet_buffer[pos].data, 1, packet_buffer[pos].size, fp);
            
            VLOG(DEBUG, "Writing packet with seqno: %d to file", next_expected_seqno);
            
            // Update next expected sequence number
            next_expected_seqno += packet_buffer[pos].size;
            
            // Mark as not received for future use
            packet_buffer[pos].received = 0;
        }
        
         /* 
         * Construct an acknowledgment (ACK) packet and send it back to the sender. 
         * This informs the sender of the highest in-order packet received.
         */
        sndpkt = make_packet(0);
        sndpkt->hdr.ackno = next_expected_seqno;
        sndpkt->hdr.ctr_flags = ACK;
        
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        }
        
         /* 
         * If we are sending a duplicate ACK (because we haven’t received the expected packet yet), log it. 
         * Otherwise, update the last acknowledgment sent.
         */
        if (next_expected_seqno == last_ack_sent) {
            VLOG(DEBUG, "Sending duplicate ACK for seqno: %d", next_expected_seqno);
        } else {
            VLOG(DEBUG, "Sending ACK for seqno: %d", next_expected_seqno);
            last_ack_sent = next_expected_seqno;
        }
        
        free(sndpkt);
    }

    return 0;
}