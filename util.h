double time_seconds(void);
int open_socket_in_udp(int port);
int open_socket_in_tcp(int port);
void set_tcp_options(int fd);
const char *addr_to_str(struct sockaddr_in &addr);
const char *time_string(void);

#define ZERO_STRUCT(s) memset((void*)&s, 0, sizeof(s))

#define MAX(a,b) ((a)>(b)?(a):(b))
