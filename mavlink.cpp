/*
  mavlink class for UDP
 */
#include "mavlink.h"

#include "libraries/mavlink2/generated/mavlink_helpers.h"
#include <sys/types.h>
#include <sys/socket.h>
#include "tdb.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>

mavlink_system_t mavlink_system = {0, 0};

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
    if (signing_required) {
        load_signing_key(key_id);
        uint32_t utc_usec;
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        utc_usec = tv.tv_sec*1000U*1000U + tv.tv_usec;
        update_signing_timestamp(utc_usec);
    }
}

bool MAVLinkUDP::receive_message(const uint8_t *buf, size_t len, mavlink_message_t &msg)
{
    mavlink_status_t status;
    status.packet_rx_drop_count = 0;
    for (size_t i=0; i<len; i++) {
        if (mavlink_parse_char(chan, buf[i], &msg, &status)) {
            return true;
        }
    }
    return false;
}

bool MAVLinkUDP::send_message(const mavlink_message_t &msg)
{
    mavlink_message_t msg2 = msg;
    if (key_id == -1) {
        // strip signing
        //msg2.incompat_flags &= ~MAVLINK_IFLAG_SIGNED;
    }
    uint8_t buf[300];
    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg2);
    if (len > 0) {
        const auto sent = send(fd, buf, len, 0);
        if (sent != len) {
            ::printf("sent=%d len=%d on %d\n", int(sent), int(len), fd);
        }
        return sent == len;
    }
    return false;
}

/*
  callback to accept unsigned (or incorrectly signed) packets
 */
extern "C" {
static bool accept_unsigned_callback(const mavlink_status_t *status, uint32_t msgId)
{
    return true;
}
}

/*
  load key from keys.tdb
 */
bool MAVLinkUDP::load_key(int key_id)
{
    auto *tdb = tdb_open(KEY_FILE, 128, 0, O_RDWR | O_CREAT, 0600);
    if (tdb == nullptr) {
        return false;
    }
    TDB_DATA k;
    k.dptr = (char *)&key_id;
    k.dsize = sizeof(int);

    auto d = tdb_fetch(tdb, k);
    if (d.dptr == nullptr || d.dsize != sizeof(key)) {
        tdb_close(tdb);
        return false;
    }
    memcpy(&key, d.dptr, sizeof(key));
    tdb_close(tdb);
    return true;
}

/*
  save key to keys.tdb
 */
bool MAVLinkUDP::save_key(int key_id, const SigningKey &key)
{
    auto *tdb = tdb_open(KEY_FILE, 1000, 0, O_RDWR | O_CREAT, 0600);
    if (tdb == nullptr) {
        return false;
    }
    TDB_DATA k;
    k.dptr = (char*)&key_id;
    k.dsize = sizeof(int);
    TDB_DATA d;
    d.dptr = (char*)&key;
    d.dsize = sizeof(key);

    int ret = tdb_store(tdb, k, d, TDB_REPLACE);
    tdb_close(tdb);
    return ret == 0;
}

/*
  load signing key
 */
void MAVLinkUDP::load_signing_key(int key_id)
{
    mavlink_status_t *status = mavlink_get_channel_status(chan);
    if (status == nullptr) {
        ::printf("Failed to load signing key - no status\n");
        return;        
    }
    if (!load_key(key_id)) {
        ::printf("Failed to find signing key for ID %d\n", key_id);
        return;
    }

    memcpy(signing.secret_key, key.secret_key, sizeof(key.secret_key));
    signing.link_id = uint8_t(chan);

    // use a timestamp 1 minute past the last recorded
    // timestamp. Combined with saving the key once every 30s this
    // prevents a window for replay attacks
    signing.timestamp = key.timestamp + 60UL * 100UL * 1000UL;
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
}

/*
  update signing timestamp. This is called when we get GPS lock
  timestamp_usec is since 1/1/1970 (the epoch)
 */
void MAVLinkUDP::update_signing_timestamp(uint64_t timestamp_usec)
{
    uint64_t signing_timestamp = (timestamp_usec / (1000*1000ULL));
    // this is the offset from 1/1/1970 to 1/1/2015
    const uint64_t epoch_offset = 1420070400;
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
    if (!load_key(key_id)) {
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
        save_key(key_id, key);
    }
}
