/*
  mavlink message definitions
 */
#pragma once

// we have separate helpers disabled to make it possible
// to select MAVLink 1.0 in the arduino GUI build
#define MAVLINK_SEPARATE_HELPERS
#define MAVLINK_NO_CONVERSION_HELPERS

#define MAVLINK_SEND_UART_BYTES(chan, buf, len) comm_send_buffer(chan, buf, len)

// 2 comm channels per available proxy connection, plus one for STATUSTEXT msgs
#define MAX_COMM2_LINKS 8
#define MAVLINK_COMM_NUM_BUFFERS (MAX_COMM2_LINKS+2)

// mavlink channel mapping
#define CHAN_COMM1 MAVLINK_COMM_0
#define CHAN_STATUSTEXT MAVLINK_COMM_1
#define CHAN_COMM2(i) mavlink_channel_t((int(MAVLINK_COMM_2)+(i)))

#define MAVLINK_MAX_PAYLOAD_LEN 255

#if defined(__GNUC__) && __GNUC__ >= 9
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif

#include "libraries/mavlink2/generated/all/version.h"
#include "libraries/mavlink2/generated/mavlink_types.h"

/// MAVLink system definition
extern mavlink_system_t mavlink_system;

void comm_send_buffer(mavlink_channel_t chan, const uint8_t *buf, uint8_t len);

#define MAVLINK_USE_CONVENIENCE_FUNCTIONS
#include "libraries/mavlink2/generated/all/mavlink.h"

