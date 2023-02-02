double time_seconds(void);
int open_socket_in(int port);
const char *addr_to_str(struct sockaddr_in &addr);

#define ZERO_STRUCT(s) memset((void*)&s, 0, sizeof(s))
