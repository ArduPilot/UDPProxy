#include <stdio.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/wait.h>

double time_seconds(void)
{
    struct timeval tval;
    gettimeofday(&tval,NULL);
    return tval.tv_sec + (tval.tv_usec*1.0e-6);
}

/*
  open a UDP socket on the given port
*/
int open_socket_in_udp(int port)
{
    struct sockaddr_in sock;
    int res;
    int one=1;

    memset(&sock,0,sizeof(sock));

#ifdef HAVE_SOCK_SIN_LEN
    sock.sin_len = sizeof(sock);
#endif
    sock.sin_port = htons(port);
    sock.sin_family = AF_INET;

    res = socket(AF_INET, SOCK_DGRAM, 0);
    if (res == -1) { 
        fprintf(stderr, "socket failed\n"); return -1; 
        return -1;
    }

    setsockopt(res,SOL_SOCKET,SO_REUSEADDR,(char *)&one,sizeof(one));

    if (bind(res, (struct sockaddr *)&sock, sizeof(sock)) < 0) { 
        return(-1); 
    }

    return res;
}

/*
  setup TCP options for a socket
*/
void set_tcp_options(int fd)
{
    int one=1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(char *)&one,sizeof(one));
    setsockopt(fd, SOL_TCP, TCP_NODELAY, &one, sizeof(one));
}

/*
  open a TCP socket on the given port
*/
int open_socket_in_tcp(int port)
{
    struct sockaddr_in sock;
    int res;

    memset(&sock,0,sizeof(sock));

#ifdef HAVE_SOCK_SIN_LEN
    sock.sin_len = sizeof(sock);
#endif
    sock.sin_port = htons(port);
    sock.sin_family = AF_INET;

    res = socket(AF_INET, SOCK_STREAM, 0);
    if (res == -1) { 
        fprintf(stderr, "socket failed\n"); return -1; 
        return -1;
    }

    set_tcp_options(res);

    if (bind(res, (struct sockaddr *)&sock, sizeof(sock)) < 0) {
        return(-1); 
    }

    if (listen(res, 8) != 0) {
	return(-1);
    }

    set_tcp_options(res);

    return res;
}

/*
  convert address to string, uses a static return buffer
 */
const char *addr_to_str(struct sockaddr_in &addr)
{
    static char str[INET_ADDRSTRLEN+1];
    inet_ntop(AF_INET, &addr.sin_addr, str, INET_ADDRSTRLEN);
    return str;
}

/*
  return time as a string, using a static buffer
 */
const char *time_string(void)
{
    time_t t = time(nullptr);
    struct tm *tm = localtime(&t);
    static char str[100] {};
    strftime(str, sizeof(str)-1, "%F %T", tm);
    return str;
}
