/*
  mavlink class for UDP

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "mavlink.h"

#include "libraries/mavlink2/generated/mavlink_helpers.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <tdb.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include "util.h"

mavlink_system_t mavlink_system = {0, 0};

bool MAVLink::got_bad_signature[MAVLINK_COMM_NUM_BUFFERS];

// unused comm_send_buffer (as we handle packets as UDP buffers)
void comm_send_buffer(mavlink_channel_t chan, const uint8_t *buf, uint8_t len)
{
}

/*
  init connection
 */
void MAVLink::init(int _fd, mavlink_channel_t _chan, bool signing_required, bool _allow_websocket, int _key_id)
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
    bad_sig_count = 0;
    allow_websocket = _allow_websocket;
    got_bad_signature[chan] = false;

    ZERO_STRUCT(signing_streams);
    ZERO_STRUCT(signing);

    if (signing_required) {
	load_signing_key();
        update_signing_timestamp();
    }

    send_fn = send;
}

bool MAVLink::receive_message(uint8_t *&buf, ssize_t &len, mavlink_message_t &msg)
{
    mavlink_status_t status {};
    status.packet_rx_drop_count = 0;
    got_bad_signature[chan] = false;
    while (len--) {
	if (mavlink_parse_char(chan, *buf++, &msg, &status)) {
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
		    if (got_bad_signature[chan]) {
                        if (periodic_warning()) {
                            switch (signing.last_status) {
                            case MAVLINK_SIGNING_STATUS_BAD_SIGNATURE:
                            default:
                                bad_sig_count++;
                                if (bad_sig_count < 3) {
                                    // silently drop it
                                    return false;
                                }
                                mav_printf(MAV_SEVERITY_CRITICAL, "Bad support signing key");
                                break;
                            case MAVLINK_SIGNING_STATUS_REPLAY:
                                bad_sig_count++;
                                if (bad_sig_count < 3) {
                                    // silently drop it
                                    return false;
                                }
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
                    bad_sig_count = 0;
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

bool MAVLink::send_message(const mavlink_message_t &msg)
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
		    send_fn(fd, buf, len, 0);
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

    // keep the sequence numbers aligned so if there are multiple system IDs we get correct
    // packet loss information
    status->current_tx_seq = msg.seq;

    mavlink_finalize_message_buffer(&msg2, msg2.sysid, msg2.compid, status, min_len, max_len, crc_extra);

    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg2);
    if (len > 0) {
	return send_fn(fd, buf, len, 0) == len;
    }
    return false;
}

/*
  callback to accept unsigned (or incorrectly signed) packets
 */
bool MAVLink::accept_unsigned_callback(const mavlink_status_t *status, uint32_t msgId)
{
    // we accept all and use status to check in receive_message()
    if (status->signing) {
	auto _chan = mavlink_channel_t(status->signing->link_id);
	if (_chan < MAVLINK_COMM_NUM_BUFFERS) {
	    got_bad_signature[_chan] = true;
	}
    }
    return true;
}

/*
  load key from keys.tdb
 */
bool MAVLink::load_key(TDB_CONTEXT *tdb)
{
    return db_load_key(tdb, key_id, key);
}

/*
  save key to keys.tdb
 */
bool MAVLink::save_key(TDB_CONTEXT *tdb)
{
    return db_save_key(tdb, key_id, key);
}

/*
  load signing key
 */
void MAVLink::load_signing_key(void)
{
    mavlink_status_t *status = mavlink_get_channel_status(chan);
    if (status == nullptr) {
        ::printf("Failed to load signing key for %d - no status\n", key_id);
        return;        
    }
    auto *db = db_open();
    // we fallback to the default key ID of 0 if no signing key
    if (!load_key(db)) {
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
void MAVLink::update_signing_timestamp()
{
    double now = time_seconds();
    if (now - last_signing_save_s < 10) {
        return;
    }
    last_signing_save_s = now;
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
	mavlink_channel_t _chan = (mavlink_channel_t)(MAVLINK_COMM_0 + i);
	mavlink_status_t *status = mavlink_get_channel_status(_chan);
        if (status && status->signing && status->signing->timestamp < signing_timestamp) {
            status->signing->timestamp = signing_timestamp;
        }
    }

    // save to stable storage as a child process to minimise latency
    // in this process
    signal(SIGCHLD, SIG_IGN);
    if (fork() == 0) {
	save_signing_timestamp();
	exit(0);
    }
}


/*
  save the signing timestamp periodically
 */
void MAVLink::save_signing_timestamp(void)
{
    auto *db = db_open_transaction();
    if (db == nullptr) {
        return;
    }
    if (!load_key(db)) {
        printf("Bad key %d\n", key_id);
        db_close_cancel(db);
        return;
    }
    bool need_save = false;

    for (uint8_t i=0; i<MAVLINK_COMM_NUM_BUFFERS; i++) {
	mavlink_channel_t _chan = (mavlink_channel_t)(MAVLINK_COMM_0 + i);
	const mavlink_status_t *status = mavlink_get_channel_status(_chan);
        if (status && status->signing && status->signing->timestamp > key.timestamp) {
            key.timestamp = status->signing->timestamp;
            need_save = true;
        }
    }
    if (need_save) {
        // save updated key
	save_key(db);
	db_close_commit(db);
    } else {
        db_close_cancel(db);
    }
}

/*
  printf via MAVLink STATUSTEXT for communicating from proxy to support engineer
 */
void MAVLink::mav_printf(uint8_t severity, const char *fmt, ...)
{
    va_list arg_list;
    char text[MAVLINK_MSG_STATUSTEXT_FIELD_TEXT_LEN+1] {};
    va_start(arg_list, fmt);
    vsnprintf(text, sizeof(text), fmt, arg_list);
    va_end(arg_list);
    mavlink_message_t msg {};
    // we use CHAN_STATUSTEXT as we don't want these messages signed,
    // as if we sign them and the signature doesn't match then
    // MissionPlanner doesn't display them
    mavlink_msg_statustext_pack_chan(last_sysid, last_compid,
				     CHAN_STATUSTEXT,
                                     &msg,
                                     severity,
                                     text,
                                     0, 0);
    uint8_t buf[300];
    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
    if (len > 0) {
        ::printf("[%d]: %s\n", key_id, text);
	send_fn(fd, buf, len, 0);
    }
}

/*
  control periodic warnings to user
 */
bool MAVLink::periodic_warning(void)
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
void MAVLink::handle_setup_signing(const mavlink_message_t &msg)
{
    mavlink_setup_signing_t packet;
    mavlink_msg_setup_signing_decode(&msg, &packet);

    auto *db = db_open_transaction();
    if (db == nullptr) {
        return;
    }

    if (!load_key(db)) {
        printf("Bad key %d\n", key_id);
        db_close_cancel(db);
        return;
    }

    key.timestamp = packet.initial_timestamp;
    memcpy(key.secret_key, packet.secret_key, 32);

    ::printf("[%d] Set new signing key\n", key_id);
    save_key(db);

    got_signed_packet = false;
    db_close_commit(db);
    load_signing_key();
}
