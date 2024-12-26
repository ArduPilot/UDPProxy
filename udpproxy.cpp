/*
  UDP (and TCP) Proxy for MAVLink, with signing support

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
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
#include "keydb.h"


struct listen_port {
    struct listen_port *next;
    int port1, port2;
    int sock1_udp, sock2_udp;
    int sock1_tcp, sock2_tcp;
    pid_t pid;
};

static struct listen_port *ports;

static uint32_t count_ports(void)
{
    uint32_t count = 0;
    for (auto *p = ports; p; p=p->next) {
        count++;
    }
    return count;
}

static bool have_port2(int port2)
{
    for (auto *p = ports; p; p=p->next) {
        if (p->port2 == port2) {
            return true;
        }
    }
    return false;
}

static void open_socket(struct listen_port *p);

/*
  add a port pair to the list
 */
static void add_port(int port1, int port2)
{
    if (have_port2(port2)) {
        // already have it
        return;
    }
    struct listen_port *p = new struct listen_port;
    p->next = ports;
    p->port1 = port1;
    p->port2 = port2;
    p->sock1_udp = -1;
    p->sock2_udp = -1;
    p->sock1_tcp = -1;
    p->sock2_tcp = -1;
    p->pid = 0;
    ports = p;
    printf("Added port %d/%d\n", port1, port2);
    open_socket(p);
}


static int handle_record(struct tdb_context *db, TDB_DATA key, TDB_DATA data, void *ptr)
{
    if (key.dsize != sizeof(int) || data.dsize != sizeof(KeyEntry)) {
        // skip it
        return 0;
    }
    struct KeyEntry k {};
    int port2 = 0;
    memcpy(&port2, key.dptr, sizeof(int));
    memcpy(&k, data.dptr, sizeof(KeyEntry));
    add_port(k.port1, port2);
    return 0;
}

static void main_loop(struct listen_port *p)
{
    unsigned char buf[10240];
    bool have_conn1=false;
    bool have_conn2=false;
    double last_pkt1=0;
    double last_pkt2=0;
    uint32_t count1=0, count2=0;
    int fdmax = -1;
    fdmax = MAX(fdmax, p->sock1_udp);
    fdmax = MAX(fdmax, p->sock2_udp);
    fdmax = MAX(fdmax, p->sock1_tcp);
    fdmax = MAX(fdmax, p->sock2_tcp);
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
        FD_SET(p->sock1_udp, &fds);
        FD_SET(p->sock2_udp, &fds);
	FD_SET(p->sock1_tcp, &fds);
	FD_SET(p->sock2_tcp, &fds);

        tval.tv_sec = 10;
        tval.tv_usec = 0;

	ret = select(fdmax+1, &fds, NULL, NULL, &tval);
        if (ret == -1 && errno == EINTR) continue;
        if (ret <= 0) break;

        now = time_seconds();

        if (FD_ISSET(p->sock1_udp, &fds)) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
	    ssize_t n = recvfrom(p->sock1_udp, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
            if (n <= 0) break;
            last_pkt1 = now;
            count1++;
            if (!have_conn1) {
                if (connect(p->sock1_udp, (struct sockaddr *)&from, fromlen) != 0) {
                    break;
                }
                mav1.init(p->sock1_udp, MAVLINK_COMM_0, false);
                have_conn1 = true;
                printf("[%d] %s have conn1 for from %s\n", unsigned(p->port2), time_string(), addr_to_str(from));
            }
            mavlink_message_t msg {};
	    if (have_conn2) {
		uint8_t *buf0 = buf;
		bool failed = false;
		while (n > 0 && mav1.receive_message(buf0, n, msg)) {
		    if (!mav2.send_message(msg)) {
			failed = true;
			break;
		    }
		}
		if (failed) {
		    break;
		}
            }
        }

        if (FD_ISSET(p->sock2_udp, &fds)) {
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
	    ssize_t n = recvfrom(p->sock2_udp, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
            if (n <= 0) break;
            last_pkt2 = now;
            count2++;
            if (!have_conn2) {
                if (connect(p->sock2_udp, (struct sockaddr *)&from, fromlen) != 0) {
                    break;
                }
                mav2.init(p->sock2_udp, MAVLINK_COMM_1, true, p->port2);
                have_conn2 = true;
                printf("[%u] %s have conn2 from %s\n", unsigned(p->port2), time_string(), addr_to_str(from));
            }
            mavlink_message_t msg {};
	    if (have_conn1) {
		uint8_t *buf0 = buf;
		bool failed = false;
		while (n > 0 && mav2.receive_message(buf0, n, msg)) {
		    if (!mav1.send_message(msg)) {
			failed = true;
			break;
		    }
		}
		if (failed) {
		    break;
		}
            }
	}

	if (FD_ISSET(p->sock1_tcp, &fds)) {
	    struct sockaddr_in from;
	    socklen_t fromlen = sizeof(from);
	    if (!have_conn1) {
		int fd2 = accept(p->sock1_tcp, (struct sockaddr *)&from, &fromlen);
		if (fd2 < 0) {
		    break;
		}
		close(p->sock1_tcp);
		p->sock1_tcp = fd2;
		fdmax = MAX(fdmax, p->sock1_tcp);
		have_conn1 = true;
		printf("[%d] %s have TCP conn1 for from %s\n", unsigned(p->port2), time_string(), addr_to_str(from));
		mav1.init(p->sock1_tcp, MAVLINK_COMM_0, false);
		last_pkt1 = now;
		continue;
	    }
	    ssize_t n = recv(p->sock1_tcp, buf, sizeof(buf), 0);
	    if (n <= 0) {
		printf("[%d] %s EOF TCP conn1\n", unsigned(p->port2), time_string());
		break;
	    }
	    last_pkt1 = now;
            count1++;
	    mavlink_message_t msg {};
	    if (have_conn2) {
		uint8_t *buf0 = buf;
		bool failed = false;
		while (n > 0 && mav1.receive_message(buf0, n, msg)) {
		    if (!mav2.send_message(msg)) {
			failed = true;
			break;
		    }
		}
		if (failed) {
		    break;
		}
            }
	}

	if (FD_ISSET(p->sock2_tcp, &fds)) {
	    struct sockaddr_in from;
	    socklen_t fromlen = sizeof(from);
	    if (!have_conn2) {
		int fd2 = accept(p->sock2_tcp, (struct sockaddr *)&from, &fromlen);
		if (fd2 < 0) {
		    break;
		}
		close(p->sock2_tcp);
		p->sock2_tcp = fd2;
		fdmax = MAX(fdmax, p->sock2_tcp);
		have_conn2 = true;
		printf("[%d] %s have TCP conn2 for from %s\n", unsigned(p->port2), time_string(), addr_to_str(from));
		mav2.init(p->sock2_tcp, MAVLINK_COMM_1, false);
		last_pkt2 = now;
		continue;
	    }
	    ssize_t n = recv(p->sock2_tcp, buf, sizeof(buf), 0);
	    if (n <= 0) {
		printf("[%d] %s EOF TCP conn2\n", unsigned(p->port2), time_string());
		break;
	    }
	    last_pkt2 = now;
	    count2++;
	    mavlink_message_t msg {};
	    if (have_conn1) {
		uint8_t *buf0 = buf;
		bool failed = false;
		while (n > 0 && mav2.receive_message(buf0, n, msg)) {
		    if (!mav1.send_message(msg)) {
			failed = true;
			break;
		    }
		}
		if (failed) {
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
        // update database
        auto *db = db_open_transaction();
        if (db != nullptr) {
            struct KeyEntry ke;
            if (db_load_key(db, p->port2, ke)) {
                ke.count1 += count1;
                ke.count2 += count2;
                ke.connections++;
                db_save_key(db, p->port2, ke);
                db_close_commit(db);
            } else {
                db_close_cancel(db);
            }
        }
    }
}

static void close_socket(int *s)
{
    if (*s != -1) {
	close(*s);
	*s = -1;
    }
}

/*
  open one socket pair
 */
static void open_socket(struct listen_port *p)
{
    close_socket(&p->sock1_udp);
    close_socket(&p->sock2_udp);
    close_socket(&p->sock1_tcp);
    close_socket(&p->sock2_tcp);

    p->sock1_udp = open_socket_in_udp(p->port1);
    if (p->sock1_udp == -1) {
	printf("[%d] Failed to open UDP port %d for\n", p->port2, p->port1);
        return;
    }
    p->sock2_udp = open_socket_in_udp(p->port2);
    if (p->sock2_udp == -1) {
	printf("[%d] Failed to open UDP port %d\n", p->port2, p->port2);
        close(p->sock1_udp);
        p->sock1_udp = -1;
        return;
    }
    p->sock1_tcp = open_socket_in_tcp(p->port1);
    if (p->sock1_tcp == -1) {
        printf("[%d] Failed to open port %d for\n", p->port2, p->port1);
        return;
    }
    p->sock2_tcp = open_socket_in_tcp(p->port2);
    if (p->sock2_tcp == -1) {
        printf("[%d] Failed to open port %d\n", p->port2, p->port2);
        close(p->sock1_tcp);
        p->sock1_tcp = -1;
        return;
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

static void reload_ports(void)
{
    auto *db = db_open();
    if (db == nullptr) {
        printf("Database not found\n");
        exit(1);
    }
    tdb_traverse(db, handle_record, nullptr);
    db_close(db);
}

/*
  wait for incoming connections
 */
static void wait_connection(void)
{
    double last_reload = time_seconds();
    while (true) {
        fd_set fds;
        int ret;
        struct timeval tval;
	int fdmax = -1;

        FD_ZERO(&fds);
        for (auto *p = ports; p; p=p->next) {
            if (p->sock1_udp != -1 && p->sock2_udp != -1 && p->pid == 0) {
                FD_SET(p->sock1_udp, &fds);
                FD_SET(p->sock2_udp, &fds);
                fdmax = MAX(fdmax, p->sock1_udp);
                fdmax = MAX(fdmax, p->sock2_udp);
            }
	    if (p->sock1_tcp != -1 && p->sock2_tcp != -1 && p->pid == 0) {
		FD_SET(p->sock1_tcp, &fds);
		FD_SET(p->sock2_tcp, &fds);
		fdmax = MAX(fdmax, p->sock1_tcp);
		fdmax = MAX(fdmax, p->sock2_tcp);
            }
        }
        tval.tv_sec = 1;
        tval.tv_usec = 0;

        ret = select(fdmax+1, &fds, NULL, NULL, &tval);
        if (ret == -1 && errno == EINTR) continue;
        if (ret <= 0) {
            check_children();

            double now = time_seconds();
            if (now - last_reload > 5) {
                last_reload = now;
                reload_ports();
            }
            continue;
        }

        for (auto *p = ports; p; p=p->next) {
            if (p->sock1_udp != -1 && p->sock2_udp != -1 && p->pid == 0) {
                if (FD_ISSET(p->sock1_udp, &fds) || FD_ISSET(p->sock2_udp, &fds)) {
                    handle_connection(p);
                }
            }
	    if (p->sock1_tcp != -1 && p->sock2_tcp != -1 && p->pid == 0) {
		if (FD_ISSET(p->sock1_tcp, &fds) || FD_ISSET(p->sock2_tcp, &fds)) {
                    handle_connection(p);
                }
            }
        }
    }
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IOLBF, 4096);
    printf("Opening sockets\n");
    auto *db = db_open();
    if (db == nullptr) {
        printf("Database not found\n");
        exit(1);
    }
    tdb_traverse(db, handle_record, nullptr);
    printf("Added %u ports\n", unsigned(count_ports()));
    db_close(db);

    wait_connection();

    return 0;
}
