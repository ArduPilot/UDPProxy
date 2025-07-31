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
#include <sys/epoll.h>

#include "mavlink.h"
#include "util.h"
#include "keydb.h"
#include "websocket.h"

#define MAX_EPOLL_EVENTS 64

struct listen_port {
    struct listen_port *next;
    int port1, port2;
    int sock1_udp, sock2_udp;
    int sock1_tcp, sock2_listen;
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

static void open_sockets(struct listen_port *p);

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
    p->sock2_listen = -1;
    p->pid = 0;
    ports = p;
    printf("Added port %d/%d\n", port1, port2);
    open_sockets(p);
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

static void close_fd(int &fd)
{
    if (fd != -1) {
	close(fd);
	fd = -1;
    }
}

class Connection2 {
public:
    int sock = -1;
    bool active = false;
    MAVLink mav;
    WebSocket *ws = nullptr;

    void close(void) {
	close_fd(sock);
	active = false;
    }
};

static void main_loop(struct listen_port *p)
{
    unsigned char buf[10240];
    bool have_conn1=false;
    double last_pkt1=0;
    double last_pkt2=0;
    uint32_t count1=0, count2=0;
    int fdmax = -1;
    /*
      we allow more than one TCP connection on the support engineer
      side, but only one UDP connection
     */
    const uint8_t max_conn2_count = MAX_COMM2_LINKS;
    uint8_t conn2_count = 0;
    MAVLink mav_blank;
    MAVLink mav1;
    Connection2 conn2[max_conn2_count];

    fdmax = MAX(fdmax, p->sock1_udp);
    fdmax = MAX(fdmax, p->sock2_udp);
    fdmax = MAX(fdmax, p->sock1_tcp);
    fdmax = MAX(fdmax, p->sock2_listen);

    while (1) {
        fd_set fds;
        int ret;
        struct timeval tval;
        double now = time_seconds();
            
        if (have_conn1 && now - last_pkt1 > 10) {
            break;
        }
	if (conn2_count>0 && now - last_pkt2 > 10) {
            break;
        }
            
	FD_ZERO(&fds);
	if (p->sock1_udp != -1) {
	    FD_SET(p->sock1_udp, &fds);
	}
	if (p->sock2_udp != -1) {
	    FD_SET(p->sock2_udp, &fds);
	}
	if (p->sock1_tcp != -1) {
	    FD_SET(p->sock1_tcp, &fds);
	}
	if (p->sock2_listen != -1) {
	    FD_SET(p->sock2_listen, &fds);
	}
	for (const auto &c2 : conn2) {
	    if (c2.sock != -1) {
		FD_SET(c2.sock, &fds);
	    }
	}

        tval.tv_sec = 10;
        tval.tv_usec = 0;

	ret = select(fdmax+1, &fds, NULL, NULL, &tval);
        if (ret == -1 && errno == EINTR) continue;
        if (ret <= 0) break;

        now = time_seconds();

	if (p->sock1_udp != -1 &&
	    FD_ISSET(p->sock1_udp, &fds)) {
	    close_fd(p->sock1_tcp);
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
		mav1.init(p->sock1_udp, CHAN_COMM1, false, false);
                have_conn1 = true;
		printf("[%d] %s have UDP conn1 for from %s\n", unsigned(p->port2), time_string(), addr_to_str(from));
            }
            mavlink_message_t msg {};
	    if (conn2_count > 0) {
		uint8_t *buf0 = buf;
		while (n > 0 && mav1.receive_message(buf0, n, msg)) {
		    for (auto &c2 : conn2) {
			if (c2.sock != -1 && !c2.mav.send_message(msg)) {
			    c2.close();
			    conn2_count--;
			}
		    }
		    if (p->sock2_udp != -1) {
			conn2[0].mav.send_message(msg);
		    }
		}
		if (conn2_count == 0) {
		    break;
		}
            }
        }

	if (p->sock2_udp != -1 &&
	    FD_ISSET(p->sock2_udp, &fds)) {
	    close_fd(p->sock2_listen);
	    struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
	    ssize_t n = recvfrom(p->sock2_udp, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
            if (n <= 0) break;
            last_pkt2 = now;
	    count2++;
	    if (conn2_count == 0) {
                if (connect(p->sock2_udp, (struct sockaddr *)&from, fromlen) != 0) {
                    break;
		}
		conn2[0].mav.init(p->sock2_udp, CHAN_COMM2(0), true, false, p->port2);
		conn2_count++;
		printf("[%u] %s have UDP conn2 from %s\n", unsigned(p->port2), time_string(), addr_to_str(from));
            }
            mavlink_message_t msg {};
	    if (have_conn1) {
		uint8_t *buf0 = buf;
		bool failed = false;
		while (n > 0 && conn2[0].mav.receive_message(buf0, n, msg)) {
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

	if (!have_conn1 &&
	    p->sock1_tcp != -1 &&
	    FD_ISSET(p->sock1_tcp, &fds)) {
	    close_fd(p->sock1_udp);
	    struct sockaddr_in from;
	    socklen_t fromlen = sizeof(from);
	    int fd2 = accept(p->sock1_tcp, (struct sockaddr *)&from, &fromlen);
	    if (fd2 < 0) {
		break;
	    }
	    set_tcp_options(fd2);
	    close(p->sock1_tcp);
	    p->sock1_tcp = fd2;
	    fdmax = MAX(fdmax, p->sock1_tcp);
	    have_conn1 = true;
	    printf("[%d] %s have TCP conn1 for from %s\n", unsigned(p->port2), time_string(), addr_to_str(from));
	    mav1.init(p->sock1_tcp, CHAN_COMM1, false, false);
	    last_pkt1 = now;
	    continue;
	}

	if (p->sock1_tcp != -1 &&
	    FD_ISSET(p->sock1_tcp, &fds)) {
	    close_fd(p->sock1_udp);
	    ssize_t n = recv(p->sock1_tcp, buf, sizeof(buf), 0);
	    if (n <= 0) {
		printf("[%d] %s EOF TCP conn1\n", unsigned(p->port2), time_string());
		break;
	    }
	    last_pkt1 = now;
            count1++;
	    mavlink_message_t msg {};
	    if (conn2_count > 0) {
		uint8_t *buf0 = buf;
		while (n > 0 && mav1.receive_message(buf0, n, msg)) {
		    if (p->sock2_udp != -1) {
			if (!conn2[0].mav.send_message(msg)) {
			    conn2[0].close();
			    conn2_count--;
			}
		    }
		    for (auto &c2 : conn2) {
			if (c2.sock != -1 && !c2.mav.send_message(msg)) {
			    c2.close();
			    conn2_count--;
			}
		    }
		}
		if (conn2_count == 0) {
		    break;
		}
            }
	}

	/*
	  check for new TCP conn2 connection
	 */
	if (p->sock2_listen != -1 &&
	    FD_ISSET(p->sock2_listen, &fds)) {
	    close_fd(p->sock2_udp);
	    struct sockaddr_in from;
	    socklen_t fromlen = sizeof(from);
	    int fd2 = accept(p->sock2_listen, (struct sockaddr *)&from, &fromlen);
	    if (fd2 < 0) {
		continue;
	    }
	    if (conn2_count >= max_conn2_count) {
		// printf("[%d] %s too many TCP connections: max %u\n", unsigned(p->port2), time_string(), unsigned(max_conn2_count));
		close(fd2);
		continue;
	    }

	    set_tcp_options(fd2);

	    uint8_t i;
	    for (i=0; i<max_conn2_count; i++) {
		if (conn2[i].sock == -1) {
		    break;
		}
	    }
	    if (i == max_conn2_count) {
		printf("[%d] %s too many TCP connections BUG: max %u\n", unsigned(p->port2), time_string(), unsigned(max_conn2_count));
		close(fd2);
		continue;
	    }
	    auto &c2 = conn2[i];
	    c2.sock = fd2;
	    c2.active = false;
	    fdmax = MAX(fdmax, c2.sock);
	    printf("[%d] %s have TCP conn2[%u] for from %s\n", unsigned(p->port2), time_string(), unsigned(i+1), addr_to_str(from));
	    c2.mav.init(c2.sock, CHAN_COMM2(i), true, true, p->port2);
	    last_pkt2 = now;
	    conn2_count++;
	    continue;
	}

	for (uint8_t i=0; i<max_conn2_count; i++) {
	    auto &c2 = conn2[i];
	    if (c2.sock == -1) {
		continue;
	    }
	    if (FD_ISSET(c2.sock, &fds)) {
		close_fd(p->sock2_udp);
		if (!c2.active && WebSocket::detect(c2.sock)) {
		    c2.ws = new WebSocket(c2.sock);
		    if (c2.ws == nullptr) {
			break;
		    }
		    c2.mav.set_ws(c2.ws);
		    printf("[%d] %s WebSocket%s conn2\n", unsigned(p->port2), time_string(), c2.ws->is_SSL()?" SSL":"");
		}
		ssize_t n;
		if (c2.ws) {
		    n = c2.ws->recv(buf, sizeof(buf)-1);
		} else {
		    n = recv(c2.sock, buf, sizeof(buf)-1, 0);
		}
		if (n <= 0) {
		    printf("[%d] %s EOF TCP conn2[%u]\n", unsigned(p->port2), time_string(), unsigned(i+1));
		    c2.close();
		    conn2_count--;
		    if (conn2_count == 0) {
			goto all_closed;
		    }
		    continue;
		}
		buf[n] = 0;
		last_pkt2 = now;
		count2++;
		c2.active = true;
		mavlink_message_t msg {};
		if (have_conn1) {
		    uint8_t *buf0 = buf;
		    bool failed = false;
		    while (n > 0 && c2.mav.receive_message(buf0, n, msg)) {
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
    }

all_closed:
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
  close all sockets
 */
static void close_sockets(struct listen_port *p)
{
    close_socket(&p->sock1_udp);
    close_socket(&p->sock2_udp);
    close_socket(&p->sock1_tcp);
    close_socket(&p->sock2_listen);
}

/*
  open one socket pair
 */
static void open_sockets(struct listen_port *p)
{
    if (p->sock1_udp == -1) {
	p->sock1_udp = open_socket_in_udp(p->port1);
	if (p->sock1_udp == -1) {
	    printf("[%d] Failed to open UDP port %d - %s\n", p->port2, p->port1, strerror(errno));
	}
    }
    if (p->sock2_udp == -1) {
	p->sock2_udp = open_socket_in_udp(p->port2);
	if (p->sock2_udp == -1) {
	    printf("[%d] Failed to open UDP port %d - %s\n", p->port2, p->port2, strerror(errno));
	}
    }
    if (p->sock1_tcp == -1) {
	p->sock1_tcp = open_socket_in_tcp(p->port1);
	if (p->sock1_tcp == -1) {
	    printf("[%d] Failed to open TCP port %d - %s\n", p->port2, p->port1, strerror(errno));
	}
    }
    if (p->sock2_listen == -1) {
	p->sock2_listen = open_socket_in_tcp(p->port2);
	if (p->sock2_listen == -1) {
	    printf("[%d] Failed to open TCP port %d - %s\n", p->port2, p->port2, strerror(errno));
	}
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
		open_sockets(p);
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
	for (auto *p2 = ports; p2; p2=p2->next) {
	    if (p2 != p) {
		close_sockets(p2);
	    }
	}
	main_loop(p);
	exit(0);
    }
    p->pid = pid;
    printf("[%d] New child %d\n", p->port2, int(p->pid));

    close_sockets(p);
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

    // see if any sockets need opening
    for (auto *p = ports; p; p=p->next) {
	if (p->pid == 0) {
	    open_sockets(p);
	}
    }
}

/*
  wait for incoming connections
 */
static void wait_connection(void)
{
    int epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        exit(1);
    }

    /*
      rebuild epoll structure for current list of connections
     */
    auto rebuild_epoll_set = [&]() {
        epoll_ctl(epfd, EPOLL_CTL_DEL, -1, nullptr); // dummy cleanup if needed
        for (auto *p = ports; p; p = p->next) {
            if (p->pid != 0) continue;

            struct epoll_event ev = {};
            ev.events = EPOLLIN;
            ev.data.ptr = p;

            if (p->sock1_udp != -1) {
                ev.data.fd = p->sock1_udp;
                epoll_ctl(epfd, EPOLL_CTL_ADD, p->sock1_udp, &ev);
            }
            if (p->sock2_udp != -1) {
                ev.data.fd = p->sock2_udp;
                epoll_ctl(epfd, EPOLL_CTL_ADD, p->sock2_udp, &ev);
            }
            if (p->sock1_tcp != -1) {
                ev.data.fd = p->sock1_tcp;
                epoll_ctl(epfd, EPOLL_CTL_ADD, p->sock1_tcp, &ev);
            }
            if (p->sock2_listen != -1) {
                ev.data.fd = p->sock2_listen;
                epoll_ctl(epfd, EPOLL_CTL_ADD, p->sock2_listen, &ev);
            }
        }
    };

    rebuild_epoll_set();

    double last_reload = time_seconds();
    struct epoll_event events[MAX_EPOLL_EVENTS];

    while (true) {
	int ret = epoll_wait(epfd, events, MAX_EPOLL_EVENTS, 1000); // 1 second timeout

        if (ret == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        if (ret == 0) {
            check_children();
            double now = time_seconds();
            if (now - last_reload > 5) {
                last_reload = now;
                reload_ports();
                close(epfd);
                epfd = epoll_create1(0);
                rebuild_epoll_set();
            }
            continue;
        }

        for (int i = 0; i < ret; i++) {
            int fd = events[i].data.fd;

            for (auto *p = ports; p; p = p->next) {
                if (p->pid != 0) continue;
                if ((p->sock1_udp == fd || p->sock2_udp == fd ||
                     p->sock1_tcp == fd || p->sock2_listen == fd)) {
                    handle_connection(p);
                    break;
                }
            }
        }
    }
    close(epfd);
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
