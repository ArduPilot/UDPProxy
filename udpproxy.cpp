/*
  UDP Proxy for MAVLink, with signing support
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include "mavlink.h"
#include "util.h"

static void main_loop(int sock1, int sock2, int listen_port1, int listen_port2)
{
    unsigned char buf[10240];
    bool have_conn1=false;
    bool have_conn2=false;
    double last_pkt1=0;
    double last_pkt2=0;
    uint32_t count1=0, count2=0;
    int fdmax = (sock1>sock2?sock1:sock2)+1;
    MAVLinkUDP mav1, mav2;

    while (1) {
        fd_set fds;
        int ret;
        struct timeval tval;
        double now = time_seconds();
            
        if (have_conn1 && now - last_pkt1 > 10) {
            break;
        }
        if (have_conn2 && now - last_pkt2 > 10) {
            break;
        }
            
        FD_ZERO(&fds);
        FD_SET(sock1, &fds);
        FD_SET(sock2, &fds);

        tval.tv_sec = 10;
        tval.tv_usec = 0;

        ret = select(fdmax, &fds, NULL, NULL, &tval);
        if (ret == -1 && errno == EINTR) continue;
        if (ret <= 0) break;

        now = time_seconds();
        fflush(stdout);
                
        if (FD_ISSET(sock1, &fds)) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int n = recvfrom(sock1, buf, sizeof(buf), 0, 
                             (struct sockaddr *)&from, &fromlen);
            if (n <= 0) break;
            last_pkt1 = now;
            count1++;
            if (!have_conn1) {
                if (connect(sock1, (struct sockaddr *)&from, fromlen) != 0) {
                    break;
                }
                mav1.init(sock1, MAVLINK_COMM_0, false);
                have_conn1 = true;
                printf("have conn1 for ID %u from %s\n", unsigned(listen_port2), addr_to_str(from));
                fflush(stdout);
            }
            mavlink_message_t msg {};
            if (have_conn2 && mav1.receive_message(buf, n, msg)) {
                if (!mav2.send_message(msg)) {
                    break;
                }
            }
        }

        if (FD_ISSET(sock2, &fds)) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int n = recvfrom(sock2, buf, sizeof(buf), 0, 
                             (struct sockaddr *)&from, &fromlen);
            if (n <= 0) break;
            last_pkt2 = now;
            count2++;
            if (!have_conn2) {
                if (connect(sock2, (struct sockaddr *)&from, fromlen) != 0) {
                    break;
                }
                mav2.init(sock2, MAVLINK_COMM_1, true, listen_port2);
                have_conn2 = true;
                printf("have conn2 for ID %u from %s\n", unsigned(listen_port2), addr_to_str(from));
                fflush(stdout);
            }
            mavlink_message_t msg {};
            if (have_conn1 && mav2.receive_message(buf, n, msg)) {
                if (!mav1.send_message(msg)) {
                    break;
                }
            }
        }
    }
    printf("Closed connection for %u count1=%u count2=%u\n",
           unsigned(listen_port2), unsigned(count1), unsigned(count2));
}

static void loop_proxy(int listen_port1, int listen_port2)
{
    while (true) {
        int sock_in1 = open_socket_in(listen_port1);
        int sock_in2 = open_socket_in(listen_port2);
        if (sock_in1 == -1 || sock_in2 == -1) {
            printf("sock on ports %d or %d failed - %s\n",
                   listen_port1, listen_port2, strerror(errno));
            fflush(stdout);
            if (sock_in1 != -1) {
                close(sock_in1);
            }
            if (sock_in2 != -1) {
                close(sock_in2);
            }
            sleep(5);
            return;
        }
        
        main_loop(sock_in1, sock_in2, listen_port1, listen_port2);
        close(sock_in1);
        close(sock_in2);
    }
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        printf("Usage: udpproxy <baseport1> <baseport2> <count>\n");
        exit(1);
    }

    int listen_port1 = atoi(argv[1]);
    int listen_port2 = atoi(argv[2]);
    int count = atoi(argv[3]);

    printf("Opening %d sockets\n", count);
    fflush(stdout);
    if (count == 1) {
        loop_proxy(listen_port1, listen_port2);
    } else {
        for (int i=0; i<count; i++) {
            if (fork() == 0) {
                loop_proxy(listen_port1+i, listen_port2+i);
            }
        }
    }
    int status=0;
    wait(&status);

    return 0;
}
