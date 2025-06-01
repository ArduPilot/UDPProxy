/*
  handle websocket connections
 */
#include "websocket.h"
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

WebSocket::WebSocket(int _fd, const char *buf)
{
    fd = _fd;
    auto len = strlen(buf);

    // parse Sec-WebSocket-Key from HTTP headers
    std::string headers(reinterpret_cast<const char *>(buf), len);
    std::string key_marker = "Sec-WebSocket-Key: ";
    size_t key_pos = headers.find(key_marker);
    if (key_pos != std::string::npos) {
        key_pos += key_marker.length();
        size_t end = headers.find("\r\n", key_pos);
	if (end != std::string::npos) {
	    std::string sec_key = headers.substr(key_pos, end - key_pos);
	    send_handshake(sec_key);
        }
    }
}

/*
  decode an incoming WebSocket packet and overwrite buf with the decoded data
  return the number of decoded payload bytes, or -1 on error
 */
ssize_t WebSocket::decode(uint8_t *buf, size_t n)
{
    if (n < 2) return -1;

    // NOTE: opcode currently unused, reserved for future handling of ping/close/etc.
    [[maybe_unused]] uint8_t opcode = buf[0] & 0x0F;
    bool masked = buf[1] & 0x80;
    uint64_t payload_len = buf[1] & 0x7F;
    size_t pos = 2;

    if (payload_len == 126) {
        if (n < 4) return -1;
        payload_len = ntohs(*(uint16_t *)(buf + pos));
        pos += 2;
    } else if (payload_len == 127) {
        if (n < 10) return -1;
        payload_len = be64toh(*(uint64_t *)(buf + pos));
        pos += 8;
    }

    if (masked) {
	if (n < pos + 4 + payload_len) {
	    return -1;
	}
	uint8_t mask[4];
	memcpy(mask, buf + pos, 4);
	pos += 4;
	for (size_t i = 0; i < payload_len; i++) {
	    buf[i] = buf[pos + i] ^ mask[i % 4];
	}
    } else {
	if (n < pos + payload_len) {
	    return -1;
	}
        memmove(buf, buf + pos, payload_len);
    }

    return payload_len;
}

/*
  helper to base64 encode input
 */
static std::string base64_encode(const uint8_t* input, size_t len)
{
    BIO *bio, *b64;
    BUF_MEM *buffer_ptr;

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, input, len);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &buffer_ptr);

    std::string result(buffer_ptr->data, buffer_ptr->length);
    BIO_free_all(bio);
    return result;
}

/*
  perform websocket handshake response
 */
void WebSocket::send_handshake(const std::string &key)
{
    const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string accept_src = key + guid;

    uint8_t sha1_hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)accept_src.c_str(), accept_src.length(), sha1_hash);

    std::string accept_val = base64_encode(sha1_hash, SHA_DIGEST_LENGTH);

    char response[512];
    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n"
             "\r\n",
	     accept_val.c_str());

    ::send(fd, response, strlen(response), 0);
}

/*
  encode a packet onto a connected WebSocket
 */
ssize_t WebSocket::send(const void *buf, size_t n)
{
    uint8_t header[10];
    size_t header_len = 0;
    header[0] = 0x82; // FIN + binary opcode

    if (n <= 125) {
        header[1] = n;
        header_len = 2;
    } else if (n <= 65535) {
        header[1] = 126;
        *(uint16_t *)(header + 2) = htons(n);
        header_len = 4;
    } else {
        header[1] = 127;
        *(uint64_t *)(header + 2) = htobe64(n);
        header_len = 10;
    }

    struct iovec iov[2];
    iov[0].iov_base = header;
    iov[0].iov_len = header_len;
    iov[1].iov_base = (void *)buf;
    iov[1].iov_len = n;

    struct msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    auto sent = sendmsg(fd, &msg, 0);
    if (sent < (ssize_t)header_len) {
	return -1;
    }
    return sent - header_len; // return number of payload bytes sent
}
