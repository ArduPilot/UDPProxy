/*
  mavlink class for UDPProxy
 */
#pragma once

#include "mavlink_msgs.h"
#include "keydb.h"

/*
  abstraction for MAVLink on UDP
 */
class MAVLinkUDP {
public:
    void init(int fd, mavlink_channel_t chan, bool signing_required, int key_id=-1);
    bool receive_message(uint8_t *&buf, ssize_t &len, mavlink_message_t &msg);
    bool send_message(const mavlink_message_t &msg);

private:
    struct KeyEntry key;
    int fd;
    mavlink_channel_t chan;
    int key_id;
    bool key_loaded = false;
    bool got_signed_packet = false;
    static bool got_bad_signature;

    mavlink_signing_streams_t signing_streams {};
    mavlink_signing_t signing {};

    // last time we saved the timestamp
    double last_signing_save_s = 0;

    // last time we warned the support engineer to fix signing
    double last_signing_warning_s = 0;

    // last source sysid and compid from a HEARTBEAT from user
    uint8_t last_sysid, last_compid;

    // count of signature errors for triggering message
    uint32_t bad_sig_count = 0;

    void load_signing_key(int key_id);
    void update_signing_timestamp(void);
    void save_signing_timestamp(void);
    bool load_key(TDB_CONTEXT *db, int key_id);
    bool save_key(TDB_CONTEXT *db, int key_id);

    bool periodic_warning(void);
    void mav_printf(uint8_t severity, const char *fmt, ...);
    static bool accept_unsigned_callback(const mavlink_status_t *status, uint32_t msgId);
    void handle_setup_signing(const mavlink_message_t &msg);
};
