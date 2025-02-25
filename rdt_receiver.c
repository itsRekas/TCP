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


/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
#define MAX_BUFFER_SIZE 65536

typedef struct {
    char data[DATA_SIZE];
    int size;
    int received;
} packet_data;

tcp_packet *recvpkt;
tcp_packet *sndpkt;

// Receiver window buffer
packet_data packet_buffer[MAX_BUFFER_SIZE];

int main(int argc, char **argv) {
    int sockfd; 
    int portno; 
    int clientlen; 
    struct sockaddr_in serveraddr; 
    struct sockaddr_in clientaddr; 
    int optval; 
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;

    //variables to check into the recieve window
    int next_expected_seqno = 0; 
    int last_ack_sent = 0;   

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

      // Initialize packet buffer
    for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
        packet_buffer[i].received = 0;
        packet_buffer[i].size = 0;
    }

    /* 
     * socket: create the parent socket 
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
     * build the server's Internet address
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
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);
    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        VLOG(DEBUG, "waiting from server \n");
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        // handle eof packet 
        if ( recvpkt->hdr.data_size == 0) {
            VLOG(INFO, "End Of File has been reached");

             // Send ACK for EOF packet
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = next_expected_seqno;
            sndpkt->hdr.ctr_flags = ACK;
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }
            
            fclose(fp);
            free(sndpkt);
            break;
        }


        int packet_seqno = recvpkt->hdr.seqno;
        int packet_size = recvpkt->hdr.data_size;
        
        /* 
         * sendto: ACK back to the client 
         */
        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, packet_size, packet_seqno);


        // Calculate the expected sequence number and buffer position
        int buffer_pos = packet_seqno % MAX_BUFFER_SIZE;

        // Store packet in buffer
        if (!packet_buffer[buffer_pos].received) {
            memcpy(packet_buffer[buffer_pos].data, recvpkt->data, packet_size);
            packet_buffer[buffer_pos].size = packet_size;
            packet_buffer[buffer_pos].received = 1;
            
            VLOG(DEBUG, "Received packet with seqno: %d, expected: %d", 
                packet_seqno, next_expected_seqno);
        }

        // Process in-order packets and write to file
        while (packet_buffer[next_expected_seqno % MAX_BUFFER_SIZE].received) {
            int pos = next_expected_seqno % MAX_BUFFER_SIZE;
            fwrite(packet_buffer[pos].data, 1, packet_buffer[pos].size, fp);
            
            VLOG(DEBUG, "Writing packet with seqno: %d to file", next_expected_seqno);
            
            // Update next expected sequence number
            next_expected_seqno += packet_buffer[pos].size;
            
            // Mark as not received for future use
            packet_buffer[pos].received = 0;
        }
        
        // Send cumulative ACK for all in-order packets received
        sndpkt = make_packet(0);
        sndpkt->hdr.ackno = next_expected_seqno;
        sndpkt->hdr.ctr_flags = ACK;
        
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        }
        
        // If sending a duplicate ACK, log it
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
        