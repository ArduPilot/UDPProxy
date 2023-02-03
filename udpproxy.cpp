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
#include "tdb.h"


struct listen_port {
    struct listen_port *next;
    int port1, port2;
    int sock1, sock2;
    pid_t pid;
};

static struct listen_port *ports;

static void main_loop(struct listen_port *p)
{
    unsigned char buf[10240];
    bool have_conn1=false;
    bool have_conn2=false;
    double last_pkt1=0;
    double last_pkt2=0;
    uint32_t count1=0, count2=0;
    int fdmax = MAX(p->sock1, p->sock2) + 1;
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
        FD_SET(p->sock1, &fds);
        FD_SET(p->sock2, &fds);

        tval.tv_sec = 10;
        tval.tv_usec = 0;

        ret = select(fdmax, &fds, NULL, NULL, &tval);
        if (ret == -1 && errno == EINTR) continue;
        if (ret <= 0) break;

        now = time_seconds();
        fflush(stdout);
                
        if (FD_ISSET(p->sock1, &fds)) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int n = recvfrom(p->sock1, buf, sizeof(buf), 0, 
                             (struct sockaddr *)&from, &fromlen);
            if (n <= 0) break;
            last_pkt1 = now;
            count1++;
            if (!have_conn1) {
                if (connect(p->sock1, (struct sockaddr *)&from, fromlen) != 0) {
                    break;
                }
                mav1.init(p->sock1, MAVLINK_COMM_0, false);
                have_conn1 = true;
                printf("[%d] %s have conn1 for from %s\n", unsigned(p->port2), time_string(), addr_to_str(from));
                fflush(stdout);
            }
            mavlink_message_t msg {};
            if (have_conn2 && mav1.receive_message(buf, n, msg)) {
                if (!mav2.send_message(msg)) {
                    break;
                }
            }
        }

        if (FD_ISSET(p->sock2, &fds)) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int n = recvfrom(p->sock2, buf, sizeof(buf), 0, 
                             (struct sockaddr *)&from, &fromlen);
            if (n <= 0) break;
            last_pkt2 = now;
            count2++;
            if (!have_conn2) {
                if (connect(p->sock2, (struct sockaddr *)&from, fromlen) != 0) {
                    break;
                }
                mav2.init(p->sock2, MAVLINK_COMM_1, true, p->port2);
                have_conn2 = true;
                printf("[%u] %s have conn2 from %s\n", unsigned(p->port2), time_string(), addr_to_str(from));
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
    if (count1 != 0 || count2 != 0) {
        printf("[%d] %s Closed connection count1=%u count2=%u\n",
               p->port2,
               time_string(),
               unsigned(count1),
               unsigned(count2));
    }
}

/*
  add a port pair to the list
 */
static void add_port(int port1, int port2)
{
    struct listen_port *p = new struct listen_port;
    p->next = ports;
    p->port1 = port1;
    p->port2 = port2;
    p->sock1 = -1;
    p->sock2 = -1;
    p->pid = 0;
    ports = p;
}

/*
  open one socket pair
 */
static void open_socket(struct listen_port *p)
{
    if (p->sock1 != -1) {
        close(p->sock1);
        p->sock1 = -1;
    }
    if (p->sock2 != -1) {
        close(p->sock2);
        p->sock2 = -1;
    }
    p->sock1 = open_socket_in(p->port1);
    if (p->sock1 == -1) {
        printf("[%d] Failed to open port %d for\n", p->port2, p->port1);
        return;
    }
    p->sock2 = open_socket_in(p->port2);
    if (p->sock2 == -1) {
        printf("[%d] Failed to open port %d\n", p->port2, p->port2);
        close(p->sock1);
        p->sock1 = -1;
        return;
    }
}

/*
  open listening sockets
 */
static void open_sockets(void)
{
    for (auto *p = ports; p; p=p->next) {
        open_socket(p);
    }
}

/*
  check for child exit
 */
static void check_children(void)
{
    int wstatus = 0;
    while (true) {
        pid_t pid = waitpid(-1, &wstatus, WNOHANG);
        if (pid <= 0) {
            break;
        }
        bool found_child = false;
        for (auto *p = ports; p; p=p->next) {
            if (p->pid == pid) {
                printf("[%d] Child %d exited\n", p->port2, int(pid));
                p->pid = 0;
                found_child = true;
                open_socket(p);
                break;
            }
        }
        if (!found_child) {
            printf("No child for %d found\n", int(pid));
        }
    }
}

/*
  handle a new connection
 */
static void handle_connection(struct listen_port *p)
{
    pid_t pid = fork();
    if (pid == 0) {
        main_loop(p);
        exit(0);
    }
    p->pid = pid;
    printf("[%d] New child %d\n", p->port2, int(p->pid));
}

/*
  wait for incoming connections
 */
static void wait_connection(void)
{
    while (true) {
        fd_set fds;
        int ret;
        struct timeval tval;
        int fdmax = 0;

        FD_ZERO(&fds);
        for (auto *p = ports; p; p=p->next) {
            if (p->sock1 != -1 && p->sock2 != -1 && p->pid == 0) {
                FD_SET(p->sock1, &fds);
                FD_SET(p->sock2, &fds);
                fdmax = MAX(fdmax, p->sock1);
                fdmax = MAX(fdmax, p->sock2);
            }
        }
        tval.tv_sec = 1;
        tval.tv_usec = 0;

        ret = select(fdmax+1, &fds, NULL, NULL, &tval);
        if (ret == -1 && errno == EINTR) continue;
        if (ret <= 0) {
            check_children();
            continue;
        }

        for (auto *p = ports; p; p=p->next) {
            if (p->sock1 != -1 && p->sock2 != -1 && p->pid == 0) {
                if (FD_ISSET(p->sock1, &fds) || FD_ISSET(p->sock2, &fds)) {
                    handle_connection(p);
                }
            }
        }
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
    for (int i=0; i<count; i++) {
        add_port(listen_port1+i, listen_port2+i);
    }

    open_sockets();
    wait_connection();

    return 0;
}
