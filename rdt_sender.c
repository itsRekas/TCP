#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include "packet.h"
#include "common.h"

#define STDIN_FD    0
#define RETRY       120  // Base timeout in milliseconds
#define MAX_BACKOFF 5    // Maximum number of backoffs

// Global variables for buffer management
int buffer_current_index = -1;  // Index of first unacknowledged packet
int buffer_end_index = 0;       // Index where next packet will be placed
int cum_seq_num = 0;            // Cumulative sequence number
int dup_ACK_count = 0;          // Count of duplicate ACKs
int last_ackno = -1;            // Last ACK number received
int retransmission_count = 0;   // Count of retransmissions for current packet
int current_timeout = RETRY;    // Current timeout value (will increase with backoff)

// Network related variables
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;  

// Buffer to store packets that have been sent but not acknowledged
packet_buffer_entry packet_buffer[WINDOW_SIZE];

// Function prototypes
void start_timer();
void stop_timer();
void init_timer(int delay, void (*sig_handler)(int));
void process_acks();
int is_buffer_full();
void add_packet_to_buffer(tcp_packet *pkt, int len);
void resend_packet(int sig);

/**
 * Check if the send buffer is full
 * @return 1 if full, 0 otherwise
 */
int is_buffer_full() {
    if (buffer_current_index == -1) {
        return 0;  // Buffer is empty
    }
    return ((buffer_end_index + 1) % WINDOW_SIZE) == buffer_current_index;
}

/**
 * Add a packet to the send buffer and transmit it
 * @param pkt Packet to be added
 * @param len Length of the data in the packet
 */
void add_packet_to_buffer(tcp_packet *pkt, int len) {
    if (is_buffer_full()) {
        return;  // Cannot add packet if buffer is full
    }

    if (buffer_current_index == -1) {
        buffer_current_index = 0;  // Buffer was empty, set starting index
    }   

    // Free any existing packet at this buffer position
    if (packet_buffer[buffer_end_index].pkt != NULL) {
        free(packet_buffer[buffer_end_index].pkt);
    }

    // Create a copy of the packet
    tcp_packet *copy = malloc(TCP_HDR_SIZE + len);
    if (copy == NULL) {
        error("malloc failed");
    }
    memcpy(copy, pkt, TCP_HDR_SIZE + len);
    
    // Set sequence number and update cumulative sequence number
    copy->hdr.seqno = cum_seq_num;
    cum_seq_num += len;
    
    // Store packet information in buffer
    packet_buffer[buffer_end_index].pkt = copy;
    packet_buffer[buffer_end_index].size = TCP_HDR_SIZE + len;
    packet_buffer[buffer_end_index].seq_no = copy->hdr.seqno;

    VLOG(DEBUG, "Sending packet %d to %s", 
        copy->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
    
    // Send the packet
    if (sendto(sockfd, packet_buffer[buffer_end_index].pkt, 
               packet_buffer[buffer_end_index].size, 0, 
               (const struct sockaddr *)&serveraddr, serverlen) < 0) {
        error("sendto");
    }

    // Update buffer end index
    buffer_end_index = (buffer_end_index + 1) % WINDOW_SIZE;
    
    // Start timer if this is the first packet in the window
    if (buffer_current_index == (buffer_end_index - 1 + WINDOW_SIZE) % WINDOW_SIZE) {
        retransmission_count = 0;
        current_timeout = RETRY;
        init_timer(current_timeout, resend_packet);
        start_timer();
    }
}

/**
 * Handler for timeout signal, retransmits unacknowledged packets
 * @param sig Signal number
 */
void resend_packet(int sig) {
    if (sig == SIGALRM) {
        VLOG(INFO, "Timeout happened");

        if (buffer_current_index != -1) {
            // Only resend the first unacknowledged packet
            if (packet_buffer[buffer_current_index].pkt != NULL) {
                VLOG(DEBUG, "Resending packet %d", packet_buffer[buffer_current_index].seq_no);
                if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
                          packet_buffer[buffer_current_index].size, 0,
                          (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
            }
        }
        
        // Implement exponential backoff
        retransmission_count++;
        if (retransmission_count <= MAX_BACKOFF) {
            current_timeout *= 2;  // Double the timeout
            init_timer(current_timeout, resend_packet);
        }
        
        // Restart timer
        start_timer();
    }
}

/**
 * Start the retransmission timer
 */
void start_timer() {
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

/**
 * Stop the retransmission timer
 */
void stop_timer() {
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

/**
 * Initialize timer with a specified delay
 * @param delay Delay in milliseconds
 * @param sig_handler Signal handler function
 */
void init_timer(int delay, void (*sig_handler)(int)) {
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

/**
 * Process incoming ACK packets
 */
void process_acks() {
    char ack_buffer[MSS_SIZE];

    // Non-blocking receive to check for ACKs
    struct sockaddr_in recv_addr;
    socklen_t recv_len = sizeof(recv_addr);
    
    fd_set readfds;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10000;  // Wait up to 10ms for ACKs
    
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    
    while (select(sockfd + 1, &readfds, NULL, NULL, &tv) > 0) {
        // Data available
        int recv_bytes = recvfrom(sockfd, ack_buffer, MSS_SIZE, 0,
                       (struct sockaddr *)&recv_addr, &recv_len);
        if (recv_bytes < 0) {
            error("recvfrom");
        }
        
        recvpkt = (tcp_packet *)ack_buffer;
        if ((recvpkt->hdr.ctr_flags & ACK) == 0) {
            continue;  // Not an ACK packet
        }
        
        int ackno = recvpkt->hdr.ackno;
        
        VLOG(DEBUG, "Received ACK %d", ackno);
        
        // Check if this is a duplicate ACK
        if (ackno == last_ackno) {
            dup_ACK_count++;
            VLOG(DEBUG, "Duplicate ACK %d (count: %d)", ackno, dup_ACK_count);
            
            if (dup_ACK_count >= 3) {
                // Fast retransmit - retransmit packet at buffer_current_index
                if (buffer_current_index != -1 && packet_buffer[buffer_current_index].pkt != NULL) {
                    VLOG(INFO, "Fast retransmit for packet %d", packet_buffer[buffer_current_index].seq_no);
                    if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
                              packet_buffer[buffer_current_index].size, 0,
                              (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                        error("sendto");
                    }
                }
                
                // Reset timer
                stop_timer();
                retransmission_count = 0;
                current_timeout = RETRY;
                init_timer(current_timeout, resend_packet);
                start_timer();
                dup_ACK_count = 0;
            }
        }
        else {
            // New ACK
            if (ackno > last_ackno) {
                last_ackno = ackno;
                dup_ACK_count = 0;
                
                // Reset retransmission count and timeout on successful ACK
                retransmission_count = 0;
                current_timeout = RETRY;
                
                int advanced = 0;
                
                // Advance window
                while (buffer_current_index != -1 && 
                       buffer_current_index != buffer_end_index && 
                       packet_buffer[buffer_current_index].seq_no + 
                       (packet_buffer[buffer_current_index].size - TCP_HDR_SIZE) <= ackno) {
                    
                    VLOG(DEBUG, "Advancing window past packet %d", packet_buffer[buffer_current_index].seq_no);
                    free(packet_buffer[buffer_current_index].pkt);
                    packet_buffer[buffer_current_index].pkt = NULL;
                    packet_buffer[buffer_current_index].size = 0;
                    packet_buffer[buffer_current_index].seq_no = 0;
                    
                    buffer_current_index = (buffer_current_index + 1) % WINDOW_SIZE;
                    advanced = 1;
                    
                    // If window is now empty, reset index
                    if (buffer_current_index == buffer_end_index) {
                        buffer_current_index = -1;  // Reset window when empty
                        break;
                    }
                }
                
                // If we advanced the window, restart the timer for the new first packet
                if (advanced && buffer_current_index != -1) {
                    stop_timer();
                    init_timer(RETRY, resend_packet);
                    start_timer();
                } else if (buffer_current_index == -1) {
                    // Window is empty, stop the timer
                    stop_timer();
                }
            }
        }
        
        // Reset for next select call
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
    }
}

/**
 * Main function
 */
int main(int argc, char **argv) {
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr, "ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    // Initialize timer, sequence number and other variables
    init_timer(RETRY, resend_packet);
    cum_seq_num = 0;
    last_ackno = -1;

    // Initialize packet buffer
    for (int i = 0; i < WINDOW_SIZE; i++) {
        packet_buffer[i].pkt = NULL;
        packet_buffer[i].size = 0;
        packet_buffer[i].seq_no = 0;
    }

    // Main sending loop
    while (1) {
        // Try to fill the buffer with packets
        while (!is_buffer_full()) {
            len = fread(buffer, 1, DATA_SIZE, fp);
            if (len <= 0) {
                VLOG(INFO, "End Of File has been reached");
                
                // Send EOF packet
                sndpkt = make_packet(0);
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                          (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
                free(sndpkt);
                
                // Wait for final ACKs
                for (int wait_count = 0; buffer_current_index != -1 && wait_count < 50; wait_count++) {
                    process_acks();
                    usleep(100000);  // Wait 100ms between checks
                }
                
                // Send EOF packet again to be sure
                sndpkt = make_packet(0);
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                          (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                    error("sendto");
                }
                free(sndpkt);
                
                return 0;  // End of transmission
            }
            
            // Create packet and add to buffer
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            add_packet_to_buffer(sndpkt, len);
            free(sndpkt);
        }
        
        // Process ACKs until there's space in the window
        do {
            process_acks();
            usleep(10000);  // Sleep 10ms to avoid busy waiting
        } while (is_buffer_full());
    }

    return 0;
}