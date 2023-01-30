/*
  mavlink class for UDPProxy
 */
#pragma once

#include "mavlink_msgs.h"

#define KEY_FILE "keys.tdb"

/*
  abstraction for MAVLink on UDP
 */
class MAVLinkUDP {
public:
    void init(int fd, mavlink_channel_t chan, bool signing_required, int key_id=-1);
    bool receive_message(const uint8_t *buf, size_t len, mavlink_message_t &msg);
    bool send_message(const mavlink_message_t &msg);

    struct SigningKey {
        uint64_t magic = 0x6b73e867a72cdd1fULL;
        uint64_t timestamp;
        uint8_t secret_key[32];
    } key;

    static bool save_key(int key_id, const struct SigningKey &key);

private:
    int fd;
    mavlink_channel_t chan;
    int key_id;
    mavlink_signing_streams_t signing_streams {};
    mavlink_signing_t signing {};
    mavlink_status_t status_out {};

    // last time we saved the timestamp
    uint32_t last_signing_save_ms = 0;

    void load_signing_key(int key_id);
    void update_signing_timestamp(uint64_t timestamp_usec);
    void save_signing_timestamp(void);
    bool load_key(int key_id);
};
