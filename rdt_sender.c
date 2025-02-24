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

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //millisecond


int buffer_current_index=0;
int buffer_end_index=0;
int cum_seq_num = 0;
int dup_ACK_count=0;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;  

packet_buffer_entry packet_buffer[WINDOW_SIZE];

int is_buffer_full() {
    return ((buffer_end_index+1)%WINDOW_SIZE)>=buffer_current_index;
}

void add_packet_to_buffer(tcp_packet **pkt, int len){
    if(is_buffer_full())return;

    if (packet_buffer[buffer_end_index].pkt != NULL) {
        free(packet_buffer[buffer_end_index].pkt);
    }

    packet_buffer[buffer_end_index].pkt=pkt;
    packet_buffer[buffer_end_index].size=TCP_HDR_SIZE+len;
    packet_buffer[buffer_end_index].sent=0;
    packet_buffer[buffer_end_index].seq_no=packet_buffer[buffer_end_index].pkt->hdr.seqno;

    VLOG(DEBUG, "Sending packet %d to %s", 
        seq_i, inet_ntoa(serveraddr.sin_addr));
    
    if (sendto(sockfd, packet_buffer[buffer_end_index].pkt, 
               packet_buffer[buffer_end_index].size, 0, 
               (const struct sockaddr *)&serveraddr, serverlen) < 0) {
        error("sendto");
    }
    
    cum_seq_num+=len;
    buffer_end_index++;
}


void resend_packet(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timeout happend");

        if (packet_buffer[buffer_current_index].pkt != NULL) {
            if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
                      packet_buffer[buffer_current_index].size, 0,
                      (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
        }
        
        // Restart timer
        stop_timer();
        start_timer();
            
    }
}


void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}


void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}


/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

void process_acks() {
    char ack_buffer[MSS_SIZE];

    // Non-blocking receive to check for ACKs
    struct sockaddr_in recv_addr;
    socklen_t recv_len = sizeof(recv_addr);
    
    fd_set readfds;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;  // Non-blocking
    
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
        if (recvpkt->hdr.ctr_flags != ACK) {
            continue;  // Not an ACK packet
        }
        
        int ackno = recvpkt->hdr.ackno;
        
        VLOG(DEBUG, "Received ACK %d", ackno);
        
        // Check if this is a duplicate ACK
        if (ackno > packet_buffer[buffer_current_index].seq_no) {
            dup_ACK_count++;
            packet_buffer[(ackno/DATA_SIZE)%WINDOW_SIZE].sent=1;
            if (dup_ACK_count >= 3) {
                // Fast retransmit - resend the first unacknowledged packet
                VLOG(INFO, "Fast retransmit for packet %d", send_base);
                if (packet_buffer[buffer_current_index].pkt != NULL) {
                    if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
                              packet_buffer[buffer_current_index].size, 0,
                              (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                        error("sendto");
                    }
                    
                    // Reset timer
                    stop_timer();
                    start_timer();
                }
                dup_ACK_count = 0;
            }
        } 
        else if(ackno<packet_buffer[buffer_current_index].seq_no){
            continue;
        }
        else {
            assert(ackno==packet_buffer[buffer_current_index].seq_no);
            packet_buffer[buffer_current_index].sent=1;
            while(packet_buffer[buffer_current_index].sent){
                memcpy(packet_buffer[buffer_current_index],0,packet_buffer[buffer_current_index].size);
                buffer_current_index=((buffer_current_index+1)%WINDOW_SIZE);
            }
        }
        
        // Reset for next select call
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }
}

int main (int argc, char **argv)
{
    int portno, len;
    int next_seqno;
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
        packet_buffer[i].sent = 0;
        packet_buffer[i].size = 0;
        packet_buffer[i].seq_no = 0;
    }

    while (1)
    {
        while(!is_buffer_full()){

            len = fread(buffer, 1, DATA_SIZE, fp);
            if ( len <= 0)
            {
                VLOG(INFO, "End Of File has been reached");
                sndpkt = make_packet(0);
                sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,
                        (const struct sockaddr *)&serveraddr, serverlen);
                break;
            }
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = cum_seq_num;
            add_packet_to_buffer(&sndpkt,len);
        }
        //Wait for ACK
        do {
            
            process_acks();

            usleep(1000);
            
            
        }while(is_buffer_full());  
            
        //     VLOG(DEBUG, "Sending packet %d to %s", 
        //             send_base, inet_ntoa(serveraddr.sin_addr));
        //     /*
        //      * If the sendto is called for the first time, the system will
        //      * will assign a random port number so that server can send its
        //      * response to the src port.
        //      */
        //     if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
        //                 ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        //     {
        //         error("sendto");
        //     }



        //     start_timer();
        //     //ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
        //     //struct sockaddr *src_addr, socklen_t *addrlen);

        //     do
        //     {
        //         if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
        //                     (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
        //         {
        //             error("recvfrom");
        //         }

        //         recvpkt = (tcp_packet *)buffer;
        //         printf("%d \n", get_data_size(recvpkt));
        //         assert(get_data_size(recvpkt) <= DATA_SIZE);
        //     }while(recvpkt->hdr.ackno < next_seqno);    //ignore duplicate ACKs
        //     stop_timer();
        //     /*resend pack if don't recv ACK */
        // } while(recvpkt->hdr.ackno != next_seqno);      

        free(sndpkt);
    }

    return 0;

}



