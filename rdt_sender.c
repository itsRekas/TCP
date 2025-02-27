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

#define RETRY 120  // Retransmission timeout in ms

// Global Variables
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct hostent *server;
struct itimerval timer;
sigset_t sigmask;

// Window Variables
int base = 0, next_seq_num = 0, last_ackno = -1;
int window_size = WINDOW_SIZE;
int dup_ack_count = 0;
int eof_sent = 0, eof_ack_received = 0;

// Packet Buffer
packet_buffer_entry packet_buffer[WINDOW_SIZE];

// Function Prototypes
void start_timer();
void stop_timer();
void init_timer(int delay, void (*sig_handler)(int));
void resend_packet(int sig);
void process_acks();
void send_eof();
int add_packet_to_buffer(tcp_packet *pkt, int len);
tcp_packet* create_packet(int data_len, int seq_num, char* data);

int main(int argc, char **argv) {
    int portno, n;
    char *hostname;
    char buf[DATA_SIZE];
    FILE *fp;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <hostname> <port> <filename>\n", argv[0]);
        exit(1);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);

    // Create socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) error("ERROR opening socket");

    // Get server info
    server = gethostbyname(hostname);
    if (!server) {
        fprintf(stderr, "ERROR, no such host as %s\n", hostname);
        exit(1);
    }

    // Setup server address
    bzero((char *)&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr, (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);
    serverlen = sizeof(serveraddr);

    // Open file
    fp = fopen(argv[3], "r");
    if (!fp) error("Error opening file");

    // Init timer & signal mask
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
    init_timer(RETRY, resend_packet);

    // Initialize buffer
    for (int i = 0; i < WINDOW_SIZE; i++) {
        packet_buffer[i].pkt = NULL;
    }

    printf("Starting transmission...\n");

    while (1) {
        // Send packets within the window
        while (next_seq_num < base + window_size && (n = fread(buf, 1, DATA_SIZE, fp)) > 0) {
            tcp_packet *pkt = create_packet(n, next_seq_num, buf);
            if (!add_packet_to_buffer(pkt, n)) {
                free(pkt);
                continue;
            }
            if (base == next_seq_num) start_timer();
            next_seq_num += n;
        }

        process_acks();

        // If EOF reached and all data acknowledged, send EOF packet
        if (feof(fp) && !eof_sent && base == next_seq_num) {
            send_eof();
        }

        // Exit once EOF is acknowledged
        if (eof_sent && last_ackno >= next_seq_num) {
            eof_ack_received = 1;
            break;
        }

        usleep(10000);  // Avoid busy waiting
    }

    fclose(fp);
    printf("Transmission complete. Total bytes sent: %d\n", next_seq_num);
    close(sockfd);
    return 0;
}

// Create a packet
tcp_packet* create_packet(int data_len, int seq_num, char* data) {
    tcp_packet *pkt = malloc(TCP_HDR_SIZE + data_len);
    if (!pkt) return NULL;
    memset(pkt, 0, TCP_HDR_SIZE + data_len);
    pkt->hdr.data_size = data_len;
    pkt->hdr.seqno = seq_num;
    if (data) memcpy(pkt->data, data, data_len);
    return pkt;
}

// Add packet to buffer and send
int add_packet_to_buffer(tcp_packet *pkt, int len) {
    int index = pkt->hdr.seqno % WINDOW_SIZE;
    if (packet_buffer[index].pkt) return 0;  // Prevent overwriting unacked packets

    packet_buffer[index].pkt = pkt;
    packet_buffer[index].size = TCP_HDR_SIZE + len;
    packet_buffer[index].seq_no = pkt->hdr.seqno;

    printf("Sending packet %d (index %d)\n", pkt->hdr.seqno, index);
    if (sendto(sockfd, pkt, TCP_HDR_SIZE + len, 0, (struct sockaddr *)&serveraddr, serverlen) < 0) {
        perror("sendto");
        return 0;
    }
    return 1;
}

// Process ACKs
void process_acks() {
    char ack_buffer[MSS_SIZE];
    struct sockaddr_in recv_addr;
    socklen_t recv_len = sizeof(recv_addr);
    ssize_t recv_bytes = recvfrom(sockfd, ack_buffer, MSS_SIZE, 0, (struct sockaddr *)&recv_addr, &recv_len);

    if (recv_bytes > 0) {
        tcp_packet *ack_pkt = (tcp_packet *)ack_buffer;
        if (ack_pkt->hdr.ctr_flags & ACK) {
            int ackno = ack_pkt->hdr.ackno;
            last_ackno = ackno;
            printf("Received ACK %d\n", ackno);

            if (ackno >= base) {
                for (int i = 0; i < WINDOW_SIZE; i++) {
                    int index = (base + i) % WINDOW_SIZE;
                    if (packet_buffer[index].pkt && packet_buffer[index].seq_no < ackno) {
                        free(packet_buffer[index].pkt);
                        packet_buffer[index].pkt = NULL;
                    }
                }
                base = ackno;
                if (base == next_seq_num) stop_timer();
                else start_timer();
                dup_ack_count = 0;
            } else {
                dup_ack_count++;
                if (dup_ack_count == 3) {
                    resend_packet(0);
                    dup_ack_count = 0;
                }
            }
        }
    }
}

// Send EOF packet
void send_eof() {
    tcp_packet *eof_pkt = make_packet(0);
    eof_pkt->hdr.seqno = next_seq_num;
    eof_pkt->hdr.ctr_flags = ACK;
    printf("Sending EOF packet with seqno: %d\n", eof_pkt->hdr.seqno);

    if (sendto(sockfd, eof_pkt, TCP_HDR_SIZE, 0, (struct sockaddr *)&serveraddr, serverlen) < 0) {
        error("sendto");
    }
    eof_sent = 1;
    free(eof_pkt);
}

// Timer functions
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

// Resend unacknowledged packet
void resend_packet(int sig) {
    if (sig == SIGALRM) {
        printf("TIMEOUT: Resending base packet %d\n", base);
        int index = base % WINDOW_SIZE;
        if (packet_buffer[index].pkt) {
            sendto(sockfd, packet_buffer[index].pkt, packet_buffer[index].size, 0, (struct sockaddr *)&serveraddr, serverlen);
            start_timer();
        }
    }
}
