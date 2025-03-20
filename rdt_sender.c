#include <stdio.h>       // Input/output functions
#include <unistd.h>      // POSIX OS API (file/socket ops)
#include <stdlib.h>      // General utilities (memory, strings)
#include <string.h>      // String manipulation functions

// Socket programming headers
#include <sys/types.h>   // Data types for system calls
#include <sys/socket.h>  // Socket programming interface
#include <netinet/in.h>  // Internet address family
#include <arpa/inet.h>   // IP address manipulation

// Signal handling headers
#include <signal.h>      // Signal handling functions
#include <sys/time.h>    // Time structures for timers

// Additional utilities
#include <time.h>        // Time functions
#include <assert.h>      // Diagnostic assertions

// Custom headers
#include "packet.h"      // TCP packet structure definition
#include "common.h"      // Shared constants and utilities

// Constants
#define STDIN_FD 0       // File descriptor for standard input
#define RETRY 120        // Retransmission timeout in milliseconds
#define INITIAL_SSTHRESH 64 // Initial slow start threshold (packets)
#define MAX_RTO 240      // Maximum RTO in seconds
#define ALPHA 0.125      // EWMA weight for RTT
#define BETA 0.25        // EWMA weight for RTT variance

//congestion control states
enum {
    SLOW_START,
    CONGESTION_AVOIDANCE
};

// Sliding window management
int buffer_current_index = 0;    // Start of the current window
int buffer_end_index = 0;        // End of the current window
int cum_seq_num = 0;             // Current cumulative sequence number

// TCP control variables
int dup_ACK_count = 0;           // Counter for duplicate ACKs (fast retransmit)-->3 for retransmit.
int eof_reached = 0;             // Flag for when end-of-file has been reached
int eof_sent = 0;                // Flag for when EOF has been sent
int timeout_ctr = 0;             // Timeout counter for backoff(if after sending eof packet which is the last packet to be sent only after every other thing has been sent and 3 timeout occur this keeps track)
int last_ackno = -1;             // Last acknowledged sequence number

// RTT estimation variables
double estimated_rtt = 0.0;      // Estimated RTT
double dev_rtt = 0.0;            // RTT deviation
double rto = 3.0;                // Retransmission timeout (initially 3 seconds)
int rto_backoff_count = 0;       // Count for exponential backoff

// Congestion control variables
float cwnd = 1.0;                // Congestion window size (initially 1 packet)
int ssthresh = 64;               // Slow start threshold
int congestion_state = SLOW_START; // Current congestion control state
struct timeval tv_last_window_change; // Time of last window change

// Network communication
int sockfd;                      // Socket file descriptor
int serverlen;                   // Server address length
struct sockaddr_in serveraddr;   // Server address structure

// Timer management
struct itimerval timer;          // Interval timer structure
tcp_packet *sndpkt;              // Outgoing packet buffer
tcp_packet *recvpkt;             // Incoming packet buffer
sigset_t sigmask;                // Signal mask for timer

// Sliding window buffer
packet_buffer_entry packet_buffer[WINDOW_SIZE]; // Buffer for unacknowledged packets

// sending timing tracking for RTT estimation 
struct timeval send_time[WINDOW_SIZE]; // Timestamp when packet was sent
int retransmitted[WINDOW_SIZE];       // Flag if packet was retransmitted

//cwnd logging 
FILE *cwnd_file; // File pointer for CWND logging


// Function declarations
void start_timer();// Starts timer
void stop_timer();// Stops timer
void init_timer(int delay, void (*sig_handler)(int));// Initializes timer
void add_packet_to_buffer(tcp_packet *pkt, int len);// Adds packets to the buffer and send them 
void process_acks();// Processes received acks and manages buffer
void resend_packet(int sig);// Resends timeout packets
int is_buffer_full(); // Checks if buffer is full
int is_buffer_empty();// Checks if buffer is empty
void update_cwnd(int ack, int duplicate);// Updates cwnd based on acks and duplicates
void update_rtt(int buffer_index, int retransmit);// Updates RTT based on buffer index and retransmission
void update_rto_on_timeout();// Updates RTO on timeout
void log_cwnd();// Logs CWND values
double calculate_backoff_rto();// Calculates RTO backoff

/**
 * Logs the current congestion window size to a CSV file.
 * Records the time since program start and the CWND value.
 */
void log_cwnd() {
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    
    // Calculate time since program start
    double time_diff = (tv_now.tv_sec - tv_last_window_change.tv_sec) + 
                      (tv_now.tv_usec - tv_last_window_change.tv_usec) / 1000000.0;
    
    // Write to CWND log file
    fprintf(cwnd_file, "%f,%f\n", time_diff, cwnd);
    fflush(cwnd_file);
    
    // Update the last window change time
    tv_last_window_change = tv_now;
}

/**
 * Updates the congestion window based on acknowledgments received.
 * Implements Slow Start and Congestion Avoidance algorithms.
 * 
 * @param ack The acknowledgment number received
 * @param duplicate Flag indicating if this is a duplicate ACK
 */
void update_cwnd(int ack, int duplicate) {
    if (duplicate) {
        // Handle duplicate ACKs
        dup_ACK_count++;
        if (dup_ACK_count >= 3) {
            // Fast Retransmit
            VLOG(INFO, "Fast Retransmit: cwnd = %f, ssthresh = %d", cwnd, ssthresh);
            
            // Update ssthresh and reset cwnd
            ssthresh = (int)fmax(cwnd / 2.0, 2.0);
            cwnd = 1.0;
            congestion_state = SLOW_START;
            
            // Log the change
            log_cwnd();
            
            // Reset duplicate ACK counter
            dup_ACK_count = 0;
        }
    } else {
        // New ACK received
        dup_ACK_count = 0;
        
        if (congestion_state == SLOW_START) {
            // Slow Start: increase cwnd by 1 for each ACK
            cwnd += 1.0;
            
            // Check if we should transition to Congestion Avoidance
            if (cwnd >= ssthresh) {
                congestion_state = CONGESTION_AVOIDANCE;
                VLOG(INFO, "Transition to Congestion Avoidance: cwnd = %f, ssthresh = %d", cwnd, ssthresh);
            }
            
            log_cwnd();
        } else if (congestion_state == CONGESTION_AVOIDANCE) {
            // Congestion Avoidance: increase cwnd by 1/cwnd for each ACK
            cwnd += 1.0 / cwnd;
            log_cwnd();
        }
    }
}

/**
 * Updates the RTT estimation based on a received ACK.
 * Implements Karn's algorithm to ignore retransmitted packets.
 * 
 * @param index The index of the packet in the buffer
 * @param is_retransmitted Flag indicating if the packet was retransmitted
 */
void update_rtt(int index, int is_retransmitted) {
    if (is_retransmitted) {
        // Karn's algorithm: Don't update RTT on retransmitted packets
        return;
    }
    
    struct timeval now;
    gettimeofday(&now, NULL);
    
    // Calculate RTT in seconds
    double measured_rtt = (now.tv_sec - send_time[index].tv_sec) + 
                         (now.tv_usec - send_time[index].tv_usec) / 1000000.0;
    
    // Update estimated RTT using EWMA
    if (estimated_rtt == 0) {
        // First measurement
        estimated_rtt = measured_rtt;
        dev_rtt = measured_rtt / 2.0;
    } else {
        // RFC 2988 updates
        dev_rtt = (1 - BETA) * dev_rtt + BETA * fabs(measured_rtt - estimated_rtt);
        estimated_rtt = (1 - ALPHA) * estimated_rtt + ALPHA * measured_rtt;
    }
    
    // Calculate RTO
    rto = estimated_rtt + 4 * dev_rtt;
    
    // Ensure RTO is within bounds
    if (rto < 1.0) rto = 1.0;
    if (rto > MAX_RTO) rto = MAX_RTO;
    
    // Reset backoff counter
    rto_backoff_count = 0;
    
    VLOG(INFO, "RTT: measured = %f, estimated = %f, dev = %f, RTO = %f", 
         measured_rtt, estimated_rtt, dev_rtt, rto);
}

/**
 * Updates the RTO when a timeout occurs.
 * Implements exponential backoff for consecutive timeouts.
 */
void update_rto_on_timeout() {
    // Increment backoff counter
    rto_backoff_count++;
    
    // Calculate backoff RTO
    double backoff_rto = calculate_backoff_rto();
    
    VLOG(INFO, "Timeout: backoff_count = %d, new RTO = %f", rto_backoff_count, backoff_rto);
    
    // Update timer values
    int delay = (int)(backoff_rto * 1000); // Convert to ms
    timer.it_interval.tv_sec = delay / 1000;
    timer.it_interval.tv_usec = (delay % 1000) * 1000;
    timer.it_value.tv_sec = delay / 1000;
    timer.it_value.tv_usec = (delay % 1000) * 1000;
}

/**
 * Calculates the RTO with exponential backoff.
 * @return The new RTO value in seconds
 */
double calculate_backoff_rto() {
    double backoff_rto = rto * (1 << rto_backoff_count);
    return fmin(backoff_rto, MAX_RTO);
}

/* Buffer status check functions for sliding window protocol */

/**
 * Checks if the circular buffer has reached maximum capacity
 * @return int - Boolean (1=full, 0=space available)
 * 
 * Implements the standard circular buffer full check formula:
 * (end + 1) % size == start indicates full condition. This
 * intentionally wastes one slot to distinguish between full 
 * and empty states.
 */
int is_buffer_full() {
    // Calculate next potential end position using modulo arithmetic
    // to wrap around the fixed-size buffer
    return ((buffer_end_index + 1) % WINDOW_SIZE) == buffer_current_index;
}

/**
 * Checks if the circular buffer is completely empty
 * @return int - Boolean (1=empty, 0=has data)
 * 
 * Uses the fundamental circular buffer property where
 * start == end indicates empty state. Relies on the
 * 'wasted slot' design pattern used in is_buffer_full()
 */
int is_buffer_empty() {
    // Direct index comparison - identical start/end positions 
    // mean no valid data between them
    return buffer_current_index == buffer_end_index;
}


/**
 * Adds a new packet to the sliding window buffer and initiates transmission.
 * It ensures in-order packet delivery by maintaining a circular buffer of unacknowledged
 * packets for potential retransmission.
 * 
 * @param pkt Pointer to the TCP packet to be sent
 * @param len Length of the packet data without headers
 */
void add_packet_to_buffer(tcp_packet *pkt, int len) {
    // Flow control check - prevent buffer overflow
    if (is_buffer_full())
        return;

    // get window size based on the congestion window 
    int effective_window = (int)cwnd;
    int window_usage = (buffer_end_index - buffer_current_index + WINDOW_SIZE) % WINDOW_SIZE;

    // Check if we're exceeding the congestion window
    if (window_usage >= effective_window && !eof_sent) {
        return;
    }

    // Track buffer state before modification for timer management
    int wasEmpty = is_buffer_empty();

    // Memory safty: Clear existing data at write position
    if (packet_buffer[buffer_end_index].pkt != NULL) {
        free(packet_buffer[buffer_end_index].pkt);
    }

    // Create owned copy of packet (prevent data races)
    tcp_packet *copy = malloc(TCP_HDR_SIZE + len);
    memcpy(copy, pkt, TCP_HDR_SIZE + len);

    // Sequence numbe management
    copy->hdr.seqno = cum_seq_num;      // Assign current cumulative sequence
    cum_seq_num += len;                 // Update global sequence tracker

    // Buffer storage with metadata
    packet_buffer[buffer_end_index].pkt = copy;
    packet_buffer[buffer_end_index].size = TCP_HDR_SIZE + len;
    packet_buffer[buffer_end_index].seq_no = copy->hdr.seqno;

    // get estimate time for RTT
    gettimeofday(&send_time[buffer_end_index], NULL);
    retransmitted[buffer_end_index] = 0;

    // Diagnostic logging (sequence tracking + destination info)
    VLOG(DEBUG, "Sending packet %d to %s",
         copy->hdr.seqno, inet_ntoa(serveraddr.sin_addr));

    // Non-blocking UDP transmission
    if (sendto(sockfd, packet_buffer[buffer_end_index].pkt,
               packet_buffer[buffer_end_index].size, 0,
               (const struct sockaddr *)&serveraddr, serverlen) < 0) {
        error("sendto");
    }

    // Circular buffer management using modulo arithmetic
    buffer_end_index = (buffer_end_index + 1) % WINDOW_SIZE;

    // Timer control logic
    if (wasEmpty) {
        start_timer();  // Only start timer if adding to empty buffer
    }
}


/**
 * Handles packet retransmission upon timer expiration (SIGALRM signal).
 * It manages timeout scenarios, handles connection termination conditions,
 * and performs packet retransmmission when necessary. The function also
 * implements a basic form of congestion control by limiting retransmission
 * attempts and adjusting the timer for the eof packet.
 *
 * @param sig Signal number (expected to be SIGALRM)
 */
void resend_packet(int sig) {
    if (sig == SIGALRM) {
        // Check if maximum retransmission attempts reached for the eof packet
        if(timeout_ctr>=3){
            stop_timer();
            return;
        }
        
        // Increment timeout counter if EOF has been sent (connection closing)
        if(eof_sent)timeout_ctr++;
        
        VLOG(INFO, "Timeout happened");//For Logging and Debugging purposes
        
        // No packets to retransmit if buffer is empty
        if (is_buffer_empty()) {
            VLOG(INFO, "Buffer empty; stopping timer");
            stop_timer();// If there are no packets stop the timer
            return;
        }
        
        // update RTO using epxonents
        update_rto_on_timeout();

        // handle the congestion control if there is a timeout 
        if (!eof_sent) {
            ssthresh = (int)fmax(cwnd / 2.0, 2.0);
            cwnd = 1.0;
            congestion_state = SLOW_START;
            log_cwnd();
        }

        // Retransmit the oldest unacknowledged packet in the buffer
        if (packet_buffer[buffer_current_index].pkt != NULL) {
            if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
                       packet_buffer[buffer_current_index].size, 0,
                       (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
            retransmitted[buffer_current_index] = 1;
        }
        
        VLOG(INFO, "RTT retransmit for packet %d",
             packet_buffer[buffer_current_index].seq_no);
        
        // Reset the timer for the next potential timeout
        stop_timer();
        start_timer();
    }
}


/**
 * Activates the retransmission timer.
 * 
 * Unblocks SIGALRM signals and starts the interval timer.
 */
void start_timer() {
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

/**
 * Deactivates the retransmission timer.
 * 
 * Blocks SIGALRM signals to prevent timer interrupts.
 */
/**
 * Deactivates the retransmission timer.
 * 
 * Blocks SIGALRM signals to prevent timer interrupts.
 */
void stop_timer() {
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
/**
 * Initializes the retransmission timer with specified delay and handler.
 * Sets up SIGALRM signal handling and configures the interval timer structure.
 */
/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
/**
 * Initializes the retransmission timer with specified delay and handler.
 * Sets up SIGALRM signal handling and configures the interval timer structure.
 */
void init_timer(int delay, void (*sig_handler)(int)) {
    signal(SIGALRM, sig_handler);

    // convert seconds to milliseconds for the timer
    int delay_ms = (int)(rto * 1000);
    if (delay_ms < delay) delay_ms = delay;

    timer.it_interval.tv_sec = delay / 1000;
    timer.it_interval.tv_usec = (delay % 1000) * 1000;
    timer.it_value.tv_sec = delay / 1000;
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


/**
 * Processes incoming ACKs and manages sliding window operations.
 * Implements TCP features: cumulative ACKs, fast retransmit on 3 dupACKs,
 * and timer management. Handles both normal ACK processing and connection
 * termination sequences.
 */
void process_acks() {
    // Network I/O buffers and structures
    char ack_buffer[MSS_SIZE];              // Storage for incoming ACK packets
    struct sockaddr_in recv_addr;           // Sender address storage
    socklen_t recv_len;                     // Address structure length
    fd_set readfds;                         // File descriptor set for select()
    struct timeval tv;                      // Timeout for non-blocking I/O

    // Continuous processing while ACKs are available
     while (1) {
        // Initialize non-blocking socket check
        FD_ZERO(&readfds);                  // Clear descriptor set
        FD_SET(sockfd, &readfds);           // Monitor socket for readability
        tv.tv_sec = 0;                      // Immediate timeout
        tv.tv_usec = 0;                     // (non-blocking check)

        // Check for available data without blocking
        if (select(sockfd + 1, &readfds, NULL, NULL, &tv) <= 0)
            break;  // Exit loop if no data available

        // Receive incoming packet
        recv_len = sizeof(recv_addr);
        int recv_bytes = recvfrom(sockfd, ack_buffer, MSS_SIZE, 0,
                                  (struct sockaddr *)&recv_addr, &recv_len);
        if (recv_bytes < 0) {
            error("recvfrom");              // Handle socket errors
        }

        // Process ACK packet
        recvpkt = (tcp_packet *)ack_buffer;
        if ((recvpkt->hdr.ctr_flags & ACK) == 0)
            continue;  // Filter out non-ACK packets

        int ackno = recvpkt->hdr.ackno;     // Extract acknowledged sequence number
        VLOG(DEBUG, "Received ACK %d", ackno);

        // Duplicate ACK handling
        int is_duplicate = (ackno == last_ackno);
        update_cwnd(ackno, is_duplicate);

        if (is_duplicate) {
            continue;  // Skip further processing for duplicate ACKs
        } else {
            last_ackno = ackno;             // Update last valid ACK received
            
            // Special handling for connection termination
            if (eof_sent) {
                stop_timer();               // Final ACK received
                buffer_current_index = buffer_end_index;  // Clear buffer
                return;
            }
            
            stop_timer();                   // Stop the timmer to prevent RTT
                        

            /* Manages sliding window advancement using cumulative acknowledgment:
            * 1. Process ACKs covering multple packets in buffer
            * 2. check if the current packet's data range (seq_no +data_length) 
            *    is fully acknowledged. Calculation breakdown:
            *    - seq_no: Start of packet data
            *    - packet_buffer[...].size-TCP_HDR_SIZE: Actual data length
            *    - sum represents end of this packet's data sequence
            * 3. Continues freing buffer slits while:
            *    a) Buffer isn't empty (has unprocessed packets)
            *    b) Current packet's entire data range is ≤ receved ACK number
            * This ensures all precceding packets are properly acknowledged before
            * moving the window forward, maintaining in-oder delivery guarantees.
            */
            while (!is_buffer_empty() &&
                (packet_buffer[buffer_current_index].seq_no +
                    (packet_buffer[buffer_current_index].size - TCP_HDR_SIZE)) <= ackno) {
                
                // Update RTT measurement
                update_rtt(buffer_current_index, retransmitted[buffer_current_index]);

                // Free acknowledged packets
                free(packet_buffer[buffer_current_index].pkt);
                packet_buffer[buffer_current_index].pkt = NULL;
                packet_buffer[buffer_current_index].size = 0;
                packet_buffer[buffer_current_index].seq_no = 0;
                
                // Move window forward with modulo arithmetic
                buffer_current_index = (buffer_current_index + 1) % WINDOW_SIZE;
            }
            
            // Update timer with new RTT estimate
            init_timer(RETRY, resend_packet);

            // Restart timer if unACKed packets are in the buffer
            if (!is_buffer_empty())
                start_timer();
        }
    }
}


int main(int argc, char **argv) {
    // Network and file I/O variables
    int portno, len;                   // Server port and read length
    char *hostname;                    // Server hostname/IP
    char buffer[DATA_SIZE];            // Data read buffer
    FILE *fp;                          // File handle for reading

    // Command line validation
    if (argc != 4) {                   // Verify 3 arguments provided
        fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    
    // Argument processing
    hostname = argv[1];                // Extract server address
    portno = atoi(argv[2]);            // Convert port string to integer
    fp = fopen(argv[3], "r");          // Open input file for reading
    if (fp == NULL) {                  // Handle file open errors
        error(argv[3]);
    }

    // Initialize CWND logging
    cwnd_file = fopen("CWND.csv", "w");
    if (cwnd_file == NULL) {
        error("Cannot open CWND.csv");
    }
    fprintf(cwnd_file, "time,cwnd\n");
    
    // Initialize time tracking
    gettimeofday(&tv_last_window_change, NULL);
    log_cwnd();  // Log initial CWND

    // Socket creation (UDP-based reliable transport)
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);  // Create UDP socket
    if (sockfd < 0) error("ERROR opening socket");

    // Server address configuration
    bzero((char *) &serveraddr, sizeof(serveraddr));  // Zero struct
    serverlen = sizeof(serveraddr);                   // Store address size
    
    // Convert hostname to binary IP format
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr, "ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    serveraddr.sin_family = AF_INET;           // IPv4 address family
    serveraddr.sin_port = htons(portno);       // Convert port to network byte order

    // Protocol validation
    assert(MSS_SIZE - TCP_HDR_SIZE > 0);  // Ensure valid data capacity

    // Initialize congestion control
    cwnd = 1.0;
    ssthresh = INITIAL_SSTHRESH;
    congestion_state = SLOW_START;
    
    // Initialize RTT estimation
    estimated_rtt = 0.0;
    dev_rtt = 0.0;
    rto = 3.0;  // Initial 3 seconds as specified


    // Protocol initialization
    init_timer(RETRY, resend_packet);  // Configure retransmission timer
    cum_seq_num = 0;                   // Initialize sequence number-could be randoom but i asked Prof Thomas and he said I should use 0

    // Clear packet buffer slots
    for (int i = 0; i < WINDOW_SIZE; i++) {
        packet_buffer[i].pkt = NULL;   // Initialize packet pointers
        packet_buffer[i].size = 0;     // Clear data sizes
        packet_buffer[i].seq_no = 0;   // Reset sequence numbers
        retransmitted[i] = 0;          // Reset retransmission flags
    }

     // Main data transfer loop
    while (1) {
        // Fill transmission window with file data
        while (!is_buffer_full() && !eof_reached) {
            // Get effective window size based on minimum of cwnd and WINDOW_SIZE
            int effective_window = (int)cwnd;
            int window_usage = (buffer_end_index - buffer_current_index + WINDOW_SIZE) % WINDOW_SIZE;
            
            // Check if we're exceeding the congestion window
            if (window_usage >= effective_window)
                break;
                
            len = fread(buffer, 1, DATA_SIZE, fp);  // Read file chunk
            if (feof(fp)) eof_reached = 1;          // if we detect eof or that we are done reading mark it with the eof-reached variable

            sndpkt = make_packet(len);              // Allocate TCP packet
            memcpy(sndpkt->data, buffer, len);      // Copy data to packet
            sndpkt->hdr.seqno = cum_seq_num;        // Set sequence number
            add_packet_to_buffer(sndpkt, len);      // Queue for transmission
            
            free(sndpkt);  // Free the packet now that it's been copied
        }

        // ACK processing loop
        do {
            process_acks();             // Handle incoming acknowledgments
            usleep(1000);               // Prevent CPU overutilization
            
            // Check effective window size
            int effective_window = (int)cwnd;
            int window_usage = (buffer_end_index - buffer_current_index + WINDOW_SIZE) % WINDOW_SIZE;
            
            // Continue if there's space in the window and data to send
            if (window_usage < effective_window && !is_buffer_full() && !eof_reached)
                break;
                
        } while (is_buffer_full() ||    // Wait while window full
                (eof_reached && !is_buffer_empty())); // Or finishing transfer that is the buffer is not empty but we are done reading from the file

        // Termination condition check
        if (eof_reached && is_buffer_empty()) break;//if the buffer is empty and we are done reading from the file break from main loop
    }

    // Post-transfer cleanup
    stop_timer();                      // Disable retransmission timer
    VLOG(INFO, "End Of File has been reached");//Logging and Debugging puporses

    // Final EOF transmission
    sndpkt = make_packet(0);           // Create zero-length EOF packet
    VLOG(INFO, "Sending last Packet");
    add_packet_to_buffer(sndpkt, 0);   // Queue EOF marker only after all the packets have been sent and we are sure of that because the buffer is empty now.
    
    // Final ACK wait with timeout
    eof_sent = 1;                  // Flag connection termination that is the eof packet has been sent
    do {
        process_acks();                // Process final acknowledgments
        usleep(1000);                  //prevent CPU overutilization
    } while (!is_buffer_empty() && timeout_ctr < 3);  // Timeout after 3 attempts... since it was the only packet in, if after 3 timeouts occur it is safe to assume that the ack for the eof was lost because we know that our receiver received the entire file for sure

    // Resource cleanup
    free(sndpkt);                      // Release EOF packet memory
    fclose(fp);                        // Close file
    return 0;                          // Successful termination
}