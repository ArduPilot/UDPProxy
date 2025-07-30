/*
  handle websocket connections
 */

#pragma once

#include <stdint.h>
#include <unistd.h>
#include <string>
#include <openssl/ssl.h>

class WebSocket {
public:
    WebSocket(int fd);

    static bool detect(int fd);
    ssize_t send(const void *buf, size_t n);
    ssize_t recv(void *buf, size_t n);
    bool is_SSL(void) const {
	return _is_SSL;
    }

private:
    int fd = -1;
    bool _is_SSL = false;
    uint8_t pending[1024] {};
    uint32_t npending = 0;
    SSL *ssl = nullptr;
    SSL_CTX *ctx = nullptr;
    bool done_headers = false;

    void fill_pending(void);
    void send_handshake(const std::string &key);
    void check_headers(void);
    ssize_t decode(uint8_t *buf, size_t n, size_t &used);

    static constexpr const char *ws_prefix = "GET / HTTP/1.1";
    static constexpr uint8_t wss_prefix[] { 0x16, 0x03, 0x01 };
};
