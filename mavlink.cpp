/*
  mavlink class for UDP
 */
#include "mavlink.h"

#include "libraries/mavlink2/generated/mavlink_helpers.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <tdb.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdlib.h>
#include "util.h"

mavlink_system_t mavlink_system = {0, 0};

bool MAVLinkUDP::got_bad_signature;

// unused comm_send_buffer (as we handle packets as UDP buffers)
void comm_send_buffer(mavlink_channel_t chan, const uint8_t *buf, uint8_t len)
{
}

/*
  init connection
 */
void MAVLinkUDP::init(int _fd, mavlink_channel_t _chan, bool signing_required, int _key_id)
{
    fd = _fd;
    chan = _chan;
    key_id = _key_id;

    got_signed_packet = false;
    key_loaded = false;
    last_signing_save_s = 0;
    last_signing_warning_s = 0;
    last_sysid = 0;
    last_compid = 0;

    ZERO_STRUCT(signing_streams);
    ZERO_STRUCT(signing);

    if (signing_required) {
        load_signing_key(key_id);
        update_signing_timestamp();
    }
}

bool MAVLinkUDP::receive_message(const uint8_t *buf, size_t len, mavlink_message_t &msg)
{
    mavlink_status_t status {};
    status.packet_rx_drop_count = 0;
    got_bad_signature = false;
    for (size_t i=0; i<len; i++) {
        if (mavlink_parse_char(chan, buf[i], &msg, &status)) {
            if (i < len-1) {
                ::printf("Multi mavlink pkt in UDP %u/%u\n", unsigned(i+1), unsigned(len));
            }
            if (key_id != -1) {
                if (!key_loaded) {
                    if (periodic_warning()) {
                        mav_printf(MAV_SEVERITY_CRITICAL, "Need to setup support signing key");
                    }
                    return false;
                } else {
                    if ((msg.incompat_flags & MAVLINK_IFLAG_SIGNED) == 0) {
                        if (periodic_warning()) {
                            mav_printf(MAV_SEVERITY_CRITICAL, "Need to use support signing key");
                        }
                        got_signed_packet = false;
                        return false;
                    }
                    if (got_bad_signature) {
                        if (periodic_warning()) {
                            switch (signing.last_status) {
                            case MAVLINK_SIGNING_STATUS_BAD_SIGNATURE:
                            default:
                                mav_printf(MAV_SEVERITY_CRITICAL, "Bad support signing key");
                                break;
                            case MAVLINK_SIGNING_STATUS_REPLAY:
                                mav_printf(MAV_SEVERITY_CRITICAL, "Bad signing timestamp - replay");
                                break;
                            case MAVLINK_SIGNING_STATUS_OLD_TIMESTAMP:
                                mav_printf(MAV_SEVERITY_CRITICAL, "Bad signing timestamp - old timestamp");
                                break;
                            case MAVLINK_SIGNING_STATUS_NO_STREAMS:
                                mav_printf(MAV_SEVERITY_CRITICAL, "Bad signing timestamp - no streams");
                                break;
                            case MAVLINK_SIGNING_STATUS_TOO_MANY_STREAMS:
                                mav_printf(MAV_SEVERITY_CRITICAL, "Bad signing timestamp - bad streams");
                                break;
                            }
                        }
                        got_signed_packet = false;
                        return false;
                    }
                    if (!got_signed_packet) {
                        got_signed_packet = true;
                        ::printf("[%d] Got good signature\n", key_id);
                    }
                    if (msg.msgid == MAVLINK_MSG_ID_SETUP_SIGNING) {
                        // handle SETUP_SIGNING locally
                        handle_setup_signing(msg);
                        return false;
                    }
                }
            }
            return true;
        }
    }
    return false;
}

bool MAVLinkUDP::send_message(const mavlink_message_t &msg)
{
    mavlink_message_t msg2 = msg;
    uint8_t buf[300];
    if (key_id == -1) {
        // strip signing
        msg2.incompat_flags &= ~MAVLINK_IFLAG_SIGNED;
    } else {
        // add signing
        msg2.incompat_flags |= MAVLINK_IFLAG_SIGNED;
        if (!got_signed_packet && msg.msgid != MAVLINK_MSG_ID_HEARTBEAT) {
            // don't send anything but HEARTBEAT until support engineer sends a signed packet
            // we return true so connection stays alive
            return true;
        }
        if (msg.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
            // remember sysid/compid for STATUSTEXT
            last_sysid = msg.sysid;
            last_compid = msg.compid;
            if (!got_signed_packet) {
                uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
                if (len > 0) {
                    send(fd, buf, len, 0);
                }
            }
        }
        if (key_loaded) {
            update_signing_timestamp();
        }
    }
    const uint8_t crc_extra = mavlink_get_crc_extra(&msg2);
    const uint8_t min_len = mavlink_min_message_length(&msg2);
    const uint8_t max_len = mavlink_max_message_length(&msg2);
    if (min_len == 0 && max_len == 0) {
        ::printf("Unknown MAVLink msg ID %u\n", unsigned(msg.msgid));
        return false;
    }
    mavlink_status_t *status = mavlink_get_channel_status(chan);
    if (status == nullptr) {
        return false;
    }
    mavlink_finalize_message_buffer(&msg2, msg2.sysid, msg2.compid, status, min_len, max_len, crc_extra);

    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg2);
    if (len > 0) {
        return send(fd, buf, len, 0) == len;
    }
    return false;
}

/*
  callback to accept unsigned (or incorrectly signed) packets
 */
bool MAVLinkUDP::accept_unsigned_callback(const mavlink_status_t *status, uint32_t msgId)
{
    // we accept all and use status to check in receive_message()
    got_bad_signature = true;
    return true;
}

/*
  load key from keys.tdb
 */
bool MAVLinkUDP::load_key(TDB_CONTEXT *tdb, int key_id)
{
    TDB_DATA k;
    k.dptr = (uint8_t *)&key_id;
    k.dsize = sizeof(int);

    auto d = tdb_fetch(tdb, k);
    if (d.dptr == nullptr || d.dsize != sizeof(key)) {
        tdb_close(tdb);
        return false;
    }
    memcpy(&key, d.dptr, sizeof(key));
    free(d.dptr);
    return key.magic == KEY_MAGIC;
}

/*
  save key to keys.tdb
 */
bool MAVLinkUDP::save_key(TDB_CONTEXT *tdb, int key_id, const KeyEntry &key)
{
    TDB_DATA k;
    k.dptr = (uint8_t*)&key_id;
    k.dsize = sizeof(int);
    TDB_DATA d;
    d.dptr = (uint8_t*)&key;
    d.dsize = sizeof(key);

    return tdb_store(tdb, k, d, TDB_REPLACE) == 0;
}

/*
  load signing key
 */
void MAVLinkUDP::load_signing_key(int key_id)
{
    mavlink_status_t *status = mavlink_get_channel_status(chan);
    if (status == nullptr) {
        ::printf("Failed to load signing key for %d - no status\n", key_id);
        return;        
    }
    auto *db = db_open();
    // we fallback to the default key ID of 0 if no signing key
    if (!load_key(db, key_id)) {
        ::printf("Failed to find signing key for ID %d\n", key_id);
        db_close(db);
        return;
    }

    key_loaded = true;

    memcpy(signing.secret_key, key.secret_key, sizeof(key.secret_key));
    signing.link_id = uint8_t(chan);

    // use a timestamp 15s past the last recorded timestamp. Combined
    // with saving the key once every 10s this prevents a window for
    // replay attacks
    signing.timestamp = key.timestamp + 15ULL * 100ULL * 1000ULL;
    signing.flags = MAVLINK_SIGNING_FLAG_SIGN_OUTGOING;
    signing.accept_unsigned_callback = accept_unsigned_callback;

    // if timestamp and key are all zero then we disable signing
    bool all_zero = (key.timestamp == 0);
    for (uint8_t i=0; i<sizeof(key.secret_key); i++) {
        if (signing.secret_key[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        // disable signing
        status->signing = nullptr;
        status->signing_streams = nullptr;
    } else {
        status->signing = &signing;
        status->signing_streams = &signing_streams;
    }
    db_close(db);
}

/*
  update signing timestamp. This is called when we get GPS lock
  timestamp_usec is since 1/1/1970 (the epoch)
 */
void MAVLinkUDP::update_signing_timestamp()
{
    double now = time_seconds();
    if (now - last_signing_save_s < 10) {
        return;
    }
    uint64_t signing_timestamp = now;
    // this is the offset from 1/1/1970 to 1/1/2015
    const uint64_t epoch_offset = 1420070400ULL;
    if (signing_timestamp > epoch_offset) {
        signing_timestamp -= epoch_offset;
    }

    // convert to 10usec units
    signing_timestamp *= 100 * 1000ULL;

    // increase signing timestamp on any links that have signing
    for (uint8_t i=0; i<MAVLINK_COMM_NUM_BUFFERS; i++) {
        mavlink_channel_t chan = (mavlink_channel_t)(MAVLINK_COMM_0 + i);
        mavlink_status_t *status = mavlink_get_channel_status(chan);
        if (status && status->signing && status->signing->timestamp < signing_timestamp) {
            status->signing->timestamp = signing_timestamp;
        }
    }

    // save to stable storage
    save_signing_timestamp();
}


/*
  save the signing timestamp periodically
 */
void MAVLinkUDP::save_signing_timestamp(void)
{
    auto *db = db_open_transaction();
    if (db == nullptr) {
        return;
    }
    if (!load_key(db, key_id)) {
        printf("Bad key %d\n", key_id);
        db_close_cancel(db);
        return;
    }
    bool need_save = false;

    for (uint8_t i=0; i<MAVLINK_COMM_NUM_BUFFERS; i++) {
        mavlink_channel_t chan = (mavlink_channel_t)(MAVLINK_COMM_0 + i);
        const mavlink_status_t *status = mavlink_get_channel_status(chan);
        if (status && status->signing && status->signing->timestamp > key.timestamp) {
            key.timestamp = status->signing->timestamp;
            need_save = true;
        }
    }
    if (need_save) {
        // save updated key
        save_key(db, key_id, key);
        db_close_commit(db);
    } else {
        db_close_cancel(db);
    }
}

/*
  printf via MAVLink STATUSTEXT for communicating from proxy to support engineer
 */
void MAVLinkUDP::mav_printf(uint8_t severity, const char *fmt, ...)
{
    va_list arg_list;
    char text[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1] {};
    va_start(arg_list, fmt);
    vsnprintf(text, sizeof(text), fmt, arg_list);
    va_end(arg_list);
    mavlink_message_t msg {};
    // we use MAVLINK_COMM_2 as we don't want these messages signed,
    // as if we sign them and the signature doesn't match then
    // MissionPlanner doesn't display them
    mavlink_msg_statustext_pack_chan(last_sysid, last_compid,
                                     MAVLINK_COMM_2,
                                     &msg,
                                     severity,
                                     text,
                                     0, 0);
    uint8_t buf[300];
    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
    if (len > 0) {
        ::printf("[%d]: %s\n", key_id, text);
        send(fd, buf, len, 0);
    }
}

/*
  control periodic warnings to user
 */
bool MAVLinkUDP::periodic_warning(void)
{
    double now = time_seconds();
    if (now - last_signing_warning_s > 2) {
        last_signing_warning_s = now;
        return true;
    }
    return false;
}

/*
  handle a (signed) SETUP_SIGNING request
  this is used to change the support signing key
 */
void MAVLinkUDP::handle_setup_signing(const mavlink_message_t &msg)
{
    mavlink_setup_signing_t packet;
    mavlink_msg_setup_signing_decode(&msg, &packet);

    auto *db = db_open_transaction();
    if (db == nullptr) {
        return;
    }

    if (!load_key(db, key_id)) {
        printf("Bad key %d\n", key_id);
        db_close_cancel(db);
        return;
    }

    key.timestamp = packet.initial_timestamp;
    memcpy(key.secret_key, packet.secret_key, 32);

    ::printf("[%d] Set new signing key\n", key_id);
    save_key(db, key_id, key);

    got_signed_packet = false;
    db_close_commit(db);
    load_signing_key(key_id);
}
