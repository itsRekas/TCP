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

#define STDIN_FD 0
#define RETRY 120 // millisecond

int buffer_current_index = 0;
int buffer_end_index = 0;
int cum_seq_num = 0;
int dup_ACK_count = 0;
int eof_reached = 0;
int eof_sent=0;
int timeout_ctr=0;

int last_ackno = -1;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;

packet_buffer_entry packet_buffer[WINDOW_SIZE];

void start_timer();
void stop_timer();
void init_timer(int delay, void (*sig_handler)(int));
void process_acks();

int is_buffer_full() {
    return ((buffer_end_index + 1) % WINDOW_SIZE) == buffer_current_index;
}

int is_buffer_empty() {
    return buffer_current_index == buffer_end_index;
}

void add_packet_to_buffer(tcp_packet *pkt, int len) {
    if (is_buffer_full())
        return;

    // Check if buffer was empty before adding.
    int wasEmpty = is_buffer_empty();

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

    if (wasEmpty) {
        start_timer();
    }
}

void resend_packet(int sig) {
    if (sig == SIGALRM) {
        if(timeout_ctr>=3){
            stop_timer();
            return;
        }
        if(eof_sent)timeout_ctr++;
        VLOG(INFO, "Timeout happened");
        if (is_buffer_empty()) {
            VLOG(INFO, "Buffer empty; stopping timer");
            stop_timer();
            return;
        }
        if (packet_buffer[buffer_current_index].pkt != NULL) {
            if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
                       packet_buffer[buffer_current_index].size, 0,
                       (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                error("sendto");
            }
        }
        VLOG(INFO, "RTT retransmit for packet %d",
             packet_buffer[buffer_current_index].seq_no);
        // Restart timer
        stop_timer();
        start_timer();
    }
}

void start_timer() {
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer() {
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
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
    socklen_t recv_len;
    fd_set readfds;
    struct timeval tv;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        if (select(sockfd + 1, &readfds, NULL, NULL, &tv) <= 0)
            break;

        recv_len = sizeof(recv_addr);
        int recv_bytes = recvfrom(sockfd, ack_buffer, MSS_SIZE, 0,
                                  (struct sockaddr *)&recv_addr, &recv_len);
        if (recv_bytes < 0) {
            error("recvfrom");
        }

        recvpkt = (tcp_packet *)ack_buffer;
        if ((recvpkt->hdr.ctr_flags & ACK) == 0)
            continue;  // Not an ACK packet

        int ackno = recvpkt->hdr.ackno;
        VLOG(DEBUG, "Received ACK %d", ackno);

        // Check if this is a duplicate ACK
        if (ackno == last_ackno) {
            dup_ACK_count++;
            if (dup_ACK_count >= 3) {
                VLOG(INFO, "Fast retransmit for packet %d",
                     packet_buffer[buffer_current_index].seq_no);
                if (packet_buffer[buffer_current_index].pkt != NULL) {
                    if (sendto(sockfd, packet_buffer[buffer_current_index].pkt,
                               packet_buffer[buffer_current_index].size, 0,
                               (const struct sockaddr *)&serveraddr, serverlen) < 0) {
                        error("sendto");
                    }
                }
                stop_timer();
                if (!is_buffer_empty())
                    start_timer();
                dup_ACK_count = 0;
            }
        } else {
            if(eof_sent){
                stop_timer();
                buffer_current_index = buffer_end_index;
                return;
            }
            stop_timer();
            last_ackno = ackno;
            dup_ACK_count = 0;
            while (!is_buffer_empty() &&
                   (packet_buffer[buffer_current_index].seq_no +
                    (packet_buffer[buffer_current_index].size - TCP_HDR_SIZE)) <= ackno) {
                free(packet_buffer[buffer_current_index].pkt);
                packet_buffer[buffer_current_index].pkt = NULL;
                packet_buffer[buffer_current_index].size = 0;
                packet_buffer[buffer_current_index].seq_no = 0;
                buffer_current_index = (buffer_current_index + 1) % WINDOW_SIZE;
            }
            if (!is_buffer_empty())
                start_timer();
        }
    }
}

int main (int argc, char **argv) {
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

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

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr, "ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    init_timer(RETRY, resend_packet);
    cum_seq_num = 0;

    for (int i = 0; i < WINDOW_SIZE; i++) {
        packet_buffer[i].pkt = NULL;
        packet_buffer[i].size = 0;
        packet_buffer[i].seq_no = 0;
    }

    while (1) {
        while (!is_buffer_full() && !eof_reached) {
            len = fread(buffer, 1, DATA_SIZE, fp);
            if (feof(fp)) {
                eof_reached = 1;
            }
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = cum_seq_num;
            add_packet_to_buffer(sndpkt, len);
        }
        // Wait for ACKs
        do {
            process_acks();
            usleep(1000);
        } while (is_buffer_full() || (eof_reached && !is_buffer_empty()));

        if (eof_reached && is_buffer_empty())
            break;
    }
    stop_timer();
    VLOG(INFO, "End Of File has been reached");

    sndpkt = make_packet(0);
    VLOG(INFO, "Sending last Packet");
    add_packet_to_buffer(sndpkt, 0);
    do {
        eof_sent=1;
        process_acks();
        usleep(1000);
    } while (!is_buffer_empty()&&timeout_ctr<3);

    free(sndpkt);
    return 0;
}
