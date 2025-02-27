#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include "packet.h"
#include "common.h"

#define STDIN_FD 0
#define RETRY 120   // Base timeout in milliseconds
#define MAX_BACKOFF 5 // Maximum number of backoffs

// Global variables
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct hostent *server;
int cum_seq_num = 0;
int last_ackno = -1;
struct itimerval timer;
sigset_t sigmask;

// Window variables
int base = 0;          // Sequence number of the oldest unacknowledged packet
int next_seq_num = 0;  // Sequence number of the next packet to send
int window_size = WINDOW_SIZE;   // Defined in common.h (default 10)
int dup_ack_count = 0;   // Count of duplicate ACKs

// Packet buffer
packet_buffer_entry packet_buffer[WINDOW_SIZE]; // Assuming this struct is correctly defined

// Function prototypes
void start_timer();
void stop_timer();
void init_timer(int delay, void (*sig_handler)(int));
void resend_packet(int sig);
void process_acks();
int add_packet_to_buffer(tcp_packet *pkt, int len); // Function to add to the buffer
tcp_packet* create_packet(int data_len, int seq_num, char* data);

int main(int argc, char **argv) {
    int portno, n;
    char *hostname;
    char buf[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr, "usage: %s <hostname> <port> <filename>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);
    serverlen = sizeof(serveraddr);

    /* get file descriptor */
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error("Error opening file");
    }

    // Initialize the signal mask
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);

    // Initialize the timer
    init_timer(RETRY, resend_packet);

    //Initialize packet buffer
    for (int i = 0; i < WINDOW_SIZE; i++) {
        packet_buffer[i].pkt = NULL; // Important: Initialize to NULL
    }

    // Main loop to read from file and send packets
    while ((n = fread(buf, 1, DATA_SIZE, fp)) > 0) {
        // Check if window is full
        if (next_seq_num < base + window_size) {
            // Create packet
            tcp_packet *sndpkt = create_packet(n, next_seq_num, buf);
            if(sndpkt == NULL) {
                error("Failed to create packet");
            }

            //Add packet to buffer and send (if space available)
            if (add_packet_to_buffer(sndpkt, n) == 0) {
                fprintf(stderr, "Error: Failed to add packet to buffer\n");
                free(sndpkt); // Free the packet if it wasn't added to the buffer
                continue; // Or handle the error as appropriate
            }

            //If it's the first packet in the window, start timer
            if (base == next_seq_num) {
              start_timer();
            }

            next_seq_num += n; // Increment next_seq_num by the amount of data
        } else {
            // Window is full, wait and process ACKs (non-blocking)
            process_acks();
            usleep(10000); // Wait for 10ms (adjust as needed)
            continue;      // Go back to the beginning of the loop
        }
        process_acks(); // Process ACKs after sending packet
    }
    fclose(fp);

     // Send End-of-File packet
    tcp_packet *eof_pkt = make_packet(0); // Zero length
    eof_pkt->hdr.seqno = next_seq_num; // Sequence number of EOF packet
    eof_pkt->hdr.ctr_flags = 0;

    VLOG(DEBUG, "Sending EOF packet with seqno: %d", eof_pkt->hdr.seqno);

    if (sendto(sockfd, eof_pkt, TCP_HDR_SIZE, 0, (const struct sockaddr *)&serveraddr, serverlen) < 0) {
        error("sendto");
    }
    // Wait for ACK of EOF
    while(last_ackno < next_seq_num) {
        process_acks();
        usleep(10000);
    }

    free(eof_pkt); // Free the EOF packet
    close(sockfd);
    return 0;
}

/**
 * create TCP packet with header and space for data of size len
 */
tcp_packet* create_packet(int data_len, int seq_num, char* data) {
    tcp_packet *pkt;
    pkt = malloc(TCP_HDR_SIZE + data_len);
    if (pkt == NULL) {
      perror("malloc");
      return NULL;
    }
    memset(pkt, 0, TCP_HDR_SIZE + data_len);  // Initialize memory to zero
    pkt->hdr.data_size = data_len;
    pkt->hdr.seqno = seq_num;
    if (data != NULL) {
        memcpy(pkt->data, data, data_len);  // Copy the data
    }
    return pkt;
}

/**
 * Adds the packet to the buffer. Returns 0 on failure, 1 on success.
 */
int add_packet_to_buffer(tcp_packet *pkt, int len) {
    int buffer_index = pkt->hdr.seqno % WINDOW_SIZE; // Index in the buffer
    if (packet_buffer[buffer_index].pkt != NULL) {
        // Buffer already has a packet, it should be acked by now.
        fprintf(stderr, "Error: Buffer overflow at index %d. Packet seqno: %d\n", buffer_index, pkt->hdr.seqno);
        return 0;
    }

    // Copy packet data into the buffer
    packet_buffer[buffer_index].pkt = pkt;
    packet_buffer[buffer_index].size = TCP_HDR_SIZE + len;
    packet_buffer[buffer_index].seq_no = pkt->hdr.seqno;

    VLOG(DEBUG, "Adding packet %d to buffer at index %d", pkt->hdr.seqno, buffer_index);

    // Send the packet
    if (sendto(sockfd, pkt, TCP_HDR_SIZE + len, 0, (const struct sockaddr *)&serveraddr, serverlen) < 0) {
        perror("sendto");
        return 0;
    }
    return 1;
}

/**
 * Process incoming ACKs.
 */
void process_acks() {
    char ack_buffer[MSS_SIZE];
    struct sockaddr_in recv_addr;
    socklen_t recv_len = sizeof(recv_addr);

    ssize_t recv_bytes = recvfrom(sockfd, ack_buffer, MSS_SIZE, 0, (struct sockaddr *)&recv_addr, &recv_len);

    if (recv_bytes > 0) {
        tcp_packet *ack_pkt = (tcp_packet *)ack_buffer;
        if (ack_pkt->hdr.ctr_flags & ACK) {
            int ackno = ack_pkt->hdr.ackno;

            VLOG(DEBUG, "Received ACK %d", ackno);
            if (ackno >= base) {
                // Acknowledge received
                int packets_acked = (ackno - base); //How many packets are being ACK'd.
                VLOG(DEBUG, "Advancing window by %d bytes", packets_acked);

                // Free all packets that have been acked
                for (int i = 0; i < WINDOW_SIZE; i++){
                    int buffer_index = (base + i) % WINDOW_SIZE;
                    if(packet_buffer[buffer_index].pkt != NULL && packet_buffer[buffer_index].seq_no < ackno) {
                        VLOG(DEBUG, "Freeing packet %d from buffer", packet_buffer[buffer_index].seq_no);
                        free(packet_buffer[buffer_index].pkt);
                        packet_buffer[buffer_index].pkt = NULL;
                    }
                }

                base = ackno;

                // Stop timer if the base has caught up with next_seq_num (window is empty)
                if (base == next_seq_num) {
                  stop_timer();
                  VLOG(DEBUG, "Window is empty, stopping timer");
                } else {
                  start_timer();
                }
                dup_ack_count = 0;
            } else {
                // Duplicate ACK
                dup_ack_count++;
                VLOG(DEBUG, "Duplicate ACK received, count = %d", dup_ack_count);

                if (dup_ack_count == 3) {
                    // Fast retransmit
                    VLOG(INFO, "Triple duplicate ACK received, performing fast retransmit");
                    resend_packet(0);  // Resend the unacknowledged packet
                    dup_ack_count = 0;  // Reset count
                }
            }
        }
    }
}

void start_timer() {
  sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
  setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer() {
  sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

void init_timer(int delay, void (*sig_handler)(int)) {
  signal(SIGALRM, sig_handler);
  timer.it_interval.tv_sec = delay / 1000;
  timer.it_interval.tv_usec = (delay % 1000) * 1000;
  timer.it_value.tv_sec = delay / 1000;
  timer.it_value.tv_usec = (delay % 1000) * 1000;
  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGALRM);
}

void resend_packet(int sig) {
    if (sig == SIGALRM) {
        VLOG(INFO, "Timeout occurred, resending packet");
        //Resend the base packet
        int buffer_index = base % WINDOW_SIZE; // Index in the buffer
        if (packet_buffer[buffer_index].pkt != NULL) {
            VLOG(DEBUG, "Resending packet %d", packet_buffer[buffer_index].pkt->hdr.seqno);
            if (sendto(sockfd, packet_buffer[buffer_index].pkt, packet_buffer[buffer_index].size, 0, (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            start_timer(); // Restart the timer
        }
    }
}

