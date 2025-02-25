// #include <stdio.h>
// #include <unistd.h>
// #include <stdlib.h>
// #include <string.h>
// #include <sys/types.h> 
// #include <sys/socket.h>
// #include <netinet/in.h>
// #include <arpa/inet.h>
// #include <signal.h>
// #include <sys/time.h>
// #include <time.h>
// #include <assert.h>

// #include"packet.h"
// #include"common.h"

// #define STDIN_FD    0
// #define RETRY  120 //millisecond


// int buffer_current_index=-1;
// int buffer_end_index=0;
// int cum_seq_num = 0;
// int dup_ACK_count=0;

// int last_ackno=-1;

// int sockfd, serverlen;
// struct sockaddr_in serveraddr;
// struct itimerval timer; 
// tcp_packet *sndpkt;
// tcp_packet *recvpkt;
// sigset_t sigmask;  

// packet_buffer_entry packet_buffer[WINDOW_SIZE];

// void start_timer();
// void stop_timer();
// void init_timer(int delay, void (*sig_handler)(int));
// void process_acks();


// int is_buffer_full() {
//     return (buffer_end_index) % WINDOW_SIZE == buffer_current_index;
// }

// void add_packet_to_buffer(tcp_packet *pkt, int len){
//     if(is_buffer_full())return;

//     if(buffer_current_index==-1){
//         buffer_current_index=0;
//     }   

//     if (packet_buffer[buffer_end_index].pkt != NULL) {
//         free(packet_buffer[buffer_end_index].pkt);
//     }

//     tcp_packet *copy = malloc(TCP_HDR_SIZE + len);
//     memcpy(copy, pkt, TCP_HDR_SIZE + len);
    
//     copy->hdr.seqno = cum_seq_num;
//     cum_seq_num += len;
    
//     packet_buffer[buffer_end_index].pkt = copy;
//     packet_buffer[buffer_end_index].size=TCP_HDR_SIZE+len;
//     packet_buffer[buffer_end_index].seq_no=copy->hdr.seqno;

//     VLOG(DEBUG, "Sending packet %d to %s", 
//         copy->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
    
//     if (sendto(sockfd, packet_buffer[buffer_end_index].pkt, 
//                packet_buffer[buffer_end_index].size, 0, 
//                (const struct sockaddr *)&serveraddr, serverlen) < 0) {
//         error("sendto");
//     }

//     buffer_end_index = (buffer_end_index + 1) % WINDOW_SIZE;
// }


// void resend_packet(int sig)
// {
//     if (sig == SIGALRM)
//     {
//         //Resend all packets range between 
//         //sendBase and nextSeqNum
//         VLOG(INFO, "Timeout happend");

//         if (packet_buffer[buffer_current_index].pkt != NULL) {
//             if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
//                       packet_buffer[buffer_current_index].size, 0,
//                       (const struct sockaddr *)&serveraddr, serverlen) < 0) {
//                 error("sendto");
//             }
//         }
        
//         // Restart timer
//         start_timer();
            
//     }
// }


// void start_timer()
// {
//     sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
//     setitimer(ITIMER_REAL, &timer, NULL);
// }


// void stop_timer()
// {
//     sigprocmask(SIG_BLOCK, &sigmask, NULL);
// }


// /*
//  * init_timer: Initialize timer
//  * delay: delay in milliseconds
//  * sig_handler: signal handler function for re-sending unACKed packets
//  */
// void init_timer(int delay, void (*sig_handler)(int)) 
// {
//     signal(SIGALRM, sig_handler);
//     timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
//     timer.it_interval.tv_usec = (delay % 1000) * 1000;  
//     timer.it_value.tv_sec = delay / 1000;       // sets an initial value
//     timer.it_value.tv_usec = (delay % 1000) * 1000;

//     sigemptyset(&sigmask);
//     sigaddset(&sigmask, SIGALRM);
// }

// void process_acks() {
//     char ack_buffer[MSS_SIZE];

//     // Non-blocking receive to check for ACKs
//     struct sockaddr_in recv_addr;
//     socklen_t recv_len = sizeof(recv_addr);
    
//     fd_set readfds;
//     struct timeval tv;
//     tv.tv_sec = 0;
//     tv.tv_usec = 0;  // Non-blocking
    
//     FD_ZERO(&readfds);
//     FD_SET(sockfd, &readfds);
    
//     while (select(sockfd + 1, &readfds, NULL, NULL, &tv) > 0) {
//         // Data available
//         int recv_bytes = recvfrom(sockfd, ack_buffer, MSS_SIZE, 0,
//                        (struct sockaddr *)&recv_addr, &recv_len);
//         if (recv_bytes < 0) {
//             error("recvfrom");
//         }
        
//         recvpkt = (tcp_packet *)ack_buffer;
//         if ((recvpkt->hdr.ctr_flags & ACK) == 0) {
//             continue;  // Not an ACK packet
//         }
        
//         int ackno = recvpkt->hdr.ackno;
        
//         VLOG(DEBUG, "Received ACK %d", ackno);
        
//         // Check if this is a duplicate ACK
//         if (ackno ==last_ackno) {
//             dup_ACK_count++;
//             if (dup_ACK_count >= 3) {
//                 // Fast retransmit - resend the first unacknowledged packet
//                 VLOG(INFO, "Fast retransmit for packet %d", packet_buffer[buffer_current_index].seq_no);
//                 if (packet_buffer[buffer_current_index].pkt != NULL) {
//                     if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
//                               packet_buffer[buffer_current_index].size, 0,
//                               (const struct sockaddr *)&serveraddr, serverlen) < 0) {
//                         error("sendto");
//                     }
                    
//                     // Reset timer
//                 }
//                 stop_timer();
//                 start_timer();
//                 dup_ACK_count = 0;
//             }
//         } 
//         else {
//             last_ackno=ackno;
//             dup_ACK_count=0;
//             while(packet_buffer[buffer_current_index].seq_no + (packet_buffer[buffer_current_index].size - TCP_HDR_SIZE) <= ackno){
//                 free(packet_buffer[buffer_current_index].pkt);
//                 packet_buffer[buffer_current_index].pkt = NULL;
//                 packet_buffer[buffer_current_index].size = 0;
//                 packet_buffer[buffer_current_index].seq_no = 0;
//                 buffer_current_index = (buffer_current_index + 1) % WINDOW_SIZE;
//             }
//         }
        
//         // Reset for next select call
//         FD_ZERO(&readfds);
//         FD_SET(sockfd, &readfds);
//         tv.tv_sec = 0;
//         tv.tv_usec = 0;
//     }
// }

// int main (int argc, char **argv)
// {
//     int portno, len;
//     char *hostname;
//     char buffer[DATA_SIZE];
//     FILE *fp;

//     /* check command line arguments */
//     if (argc != 4) {
//         fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
//         exit(0);
//     }
//     hostname = argv[1];
//     portno = atoi(argv[2]);
//     fp = fopen(argv[3], "r");
//     if (fp == NULL) {
//         error(argv[3]);
//     }

//     /* socket: create the socket */
//     sockfd = socket(AF_INET, SOCK_DGRAM, 0);
//     if (sockfd < 0) 
//         error("ERROR opening socket");


//     /* initialize server server details */
//     bzero((char *) &serveraddr, sizeof(serveraddr));
//     serverlen = sizeof(serveraddr);

//     /* covert host into network byte order */
//     if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
//         fprintf(stderr,"ERROR, invalid host %s\n", hostname);
//         exit(0);
//     }

//     /* build the server's Internet address */
//     serveraddr.sin_family = AF_INET;
//     serveraddr.sin_port = htons(portno);

//     assert(MSS_SIZE - TCP_HDR_SIZE > 0);

//     //Stop and wait protocol

//     init_timer(RETRY, resend_packet);
//     cum_seq_num = 0;

//     // Initialize packet buffer
//     for (int i = 0; i < WINDOW_SIZE; i++) {
//         packet_buffer[i].pkt = NULL;
//         packet_buffer[i].size = 0;
//         packet_buffer[i].seq_no = 0;
//     }

//     while (1)
//     {
//         while(!is_buffer_full()){
//         len = fread(buffer, 1, DATA_SIZE, fp);
//         if (len <= 0) 
//         {
//             VLOG(INFO, "End Of File has been reached");
//             sndpkt = make_packet(0);
//             sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
//                 (const struct sockaddr *)&serveraddr, serverlen);
//             free(sndpkt);
//             // Add a return statement or break the outer loop
//             return 0; // This will exit the program
//             // OR use: goto end_transmission; // And add a label at the end
//         }
//         sndpkt = make_packet(len);
//         memcpy(sndpkt->data, buffer, len);
//         sndpkt->hdr.seqno = cum_seq_num;
//         add_packet_to_buffer(sndpkt,len);
//         }
//         start_timer();
//         //Wait for ACK
//         do {
            
//             process_acks();

//             usleep(1000);
            
            
//         }while(is_buffer_full());       

//         free(sndpkt);
//     }

//     return 0;

// }

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
#define RETRY  120 //millisecond

int buffer_current_index = -1;
int buffer_end_index = 0;
int cum_seq_num = 0;
int dup_ACK_count = 0;
int last_ackno = -1;
int eof_sent = 0;
int eof_acked = 0;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
sigset_t sigmask;  

packet_buffer_entry packet_buffer[WINDOW_SIZE];

void start_timer();
void stop_timer();
void init_timer(int delay, void (*sig_handler)(int));
void process_acks();

// Fixed buffer full condition
int is_buffer_full() {
    return (buffer_end_index + 1) % WINDOW_SIZE == buffer_current_index;
}

void add_packet_to_buffer(tcp_packet *pkt, int len) {
    if(is_buffer_full()) return;

    if(buffer_current_index == -1) {
        buffer_current_index = 0;
    }   

    if (packet_buffer[buffer_end_index].pkt != NULL) {
        free(packet_buffer[buffer_end_index].pkt);
    }

    tcp_packet *copy = malloc(TCP_HDR_SIZE + len);
    memcpy(copy, pkt, TCP_HDR_SIZE + len);
    
    copy->hdr.seqno = cum_seq_num;
    cum_seq_num += len;
    
    packet_buffer[buffer_end_index].pkt = copy;
    packet_buffer[buffer_end_index].size = TCP_HDR_SIZE + len;
    packet_buffer[buffer_end_index].seq_no = copy->hdr.seqno;

    VLOG(DEBUG, "Sending packet %d to %s", 
        copy->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
    
    if (sendto(sockfd, packet_buffer[buffer_end_index].pkt, 
               packet_buffer[buffer_end_index].size, 0, 
               (const struct sockaddr *)&serveraddr, serverlen) < 0) {
        error("sendto");
    }

    buffer_end_index = (buffer_end_index + 1) % WINDOW_SIZE;
}

void resend_packet(int sig) {
    if (sig == SIGALRM) {
        VLOG(INFO, "Timeout occurred");
        if (packet_buffer[buffer_current_index].pkt != NULL) {
            if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
                      packet_buffer[buffer_current_index].size, 0,
                      (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
        }
        start_timer(); // Restart timer after timeout
    }
}

// Timer control functions
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

void process_acks() {
    char ack_buffer[MSS_SIZE];
    struct sockaddr_in recv_addr;
    socklen_t recv_len = sizeof(recv_addr);
    
    fd_set readfds;
    struct timeval tv = {0, 0};
    
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    
    while (select(sockfd + 1, &readfds, NULL, NULL, &tv) > 0) {
        int recv_bytes = recvfrom(sockfd, ack_buffer, MSS_SIZE, 0,
                       (struct sockaddr *)&recv_addr, &recv_len);
        if (recv_bytes < 0) error("recvfrom");
        
        tcp_packet *recvpkt = (tcp_packet *)ack_buffer;
        if ((recvpkt->hdr.ctr_flags & ACK) == 0) continue;
        
        int ackno = recvpkt->hdr.ackno;
        VLOG(DEBUG, "Received ACK %d", ackno);

        if (ackno == last_ackno) {
            if (++dup_ACK_count >= 3) {
                VLOG(INFO, "Fast retransmit for packet %d", 
                    packet_buffer[buffer_current_index].seq_no);
                
                if (packet_buffer[buffer_current_index].pkt != NULL) {
                    sendto(sockfd, packet_buffer[buffer_current_index].pkt,
                          packet_buffer[buffer_current_index].size, 0,
                          (const struct sockaddr *)&serveraddr, serverlen);
                }
                stop_timer();
                start_timer();
                dup_ACK_count = 0;
            }
        } else {
            last_ackno = ackno;
            dup_ACK_count = 0;
            
            while (buffer_current_index != buffer_end_index && 
                   packet_buffer[buffer_current_index].seq_no + 
                   (packet_buffer[buffer_current_index].size - TCP_HDR_SIZE) <= ackno) {
                free(packet_buffer[buffer_current_index].pkt);
                packet_buffer[buffer_current_index].pkt = NULL;
                buffer_current_index = (buffer_current_index + 1) % WINDOW_SIZE;
            }
            
            if (buffer_current_index != buffer_end_index) {
                stop_timer();
                start_timer();
            }
        }
        
        // Check EOF ACK
        if (eof_sent && ackno == cum_seq_num) {
            eof_acked = 1;
        }

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }
}

int main(int argc, char **argv) {
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
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
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol

    init_timer(RETRY, resend_packet);
    cum_seq_num = 0;

    // Initialize packet buffer
    for (int i = 0; i < WINDOW_SIZE; i++) {
        packet_buffer[i].pkt = NULL;
        packet_buffer[i].size = 0;
        packet_buffer[i].seq_no = 0;
    }
    while (1) {
        // Send packets until buffer is full
        while (!is_buffer_full() && !eof_sent) {
            len = fread(buffer, 1, DATA_SIZE, fp);
            if (len <= 0) {
                VLOG(INFO, "End Of File reached");
                tcp_packet *eof_pkt = make_packet(0);
                eof_pkt->hdr.seqno = cum_seq_num;
                add_packet_to_buffer(eof_pkt, 0);
                eof_sent = 1;
                free(eof_pkt);
                break;
            }
            
            tcp_packet *sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            add_packet_to_buffer(sndpkt, len);
            free(sndpkt);
        }

        // Start timer if needed
        if (buffer_current_index != -1 && !eof_acked) {
            start_timer();
        }

        // Process ACKs
        do {
            process_acks();
            usleep(1000);
        } while (is_buffer_full() && !eof_acked);

        if (eof_acked) {
            VLOG(INFO, "Final ACK received, exiting");
            break;
        }
    }

    // Cleanup
    for (int i = 0; i < WINDOW_SIZE; i++) {
        if (packet_buffer[i].pkt != NULL) {
            free(packet_buffer[i].pkt);
        }
    }
    fclose(fp);
    return 0;
}
