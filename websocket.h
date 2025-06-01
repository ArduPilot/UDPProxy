/*
  handle websocket connections
 */

#include <stdint.h>
#include <unistd.h>
#include <string>

class WebSocket {
public:
    WebSocket(int fd, const char *request);

    ssize_t decode(uint8_t *buf, size_t n);
    ssize_t send(const void *buf, size_t n);

private:
    int fd;
    void send_handshake(const std::string &key);
};
