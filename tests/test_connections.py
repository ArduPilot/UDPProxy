"""
Connection Tests for UDPProxy using PyMAVLink with authentication.
Tests UDP, TCP, and mixed connection scenarios with proper MAVLink2 authentication.
"""
from pymavlink import mavutil
import sys
import os
import time
import pytest
import threading
from test_config import (TEST_PORTS, TEST_PASSPHRASE,
                         MAX_TCP_ENGINEER_CONNECTIONS,
                         MULTIPLE_CONNECTIONS_TEST_DURATION)

# Set up environment for pymavlink
os.environ['MAVLINK_DIALECT'] = 'ardupilotmega'
os.environ['MAVLINK20'] = '1'  # Ensure MAVLink2 is used


def passphrase_to_key(passphrase):
    '''convert a passphrase to a 32 byte key'''
    import hashlib
    h = hashlib.new('sha256')
    if sys.version_info[0] >= 3:
        passphrase = passphrase.encode('ascii')
    h.update(passphrase)
    return h.digest()


class BaseConnectionTest:
    """Base class for connection tests with shared functionality."""

    def wait_for_connection_close(self, test_server, timeout=10):
        """Wait for UDPProxy to log 'Closed connection' indicating 
        proper cleanup."""
        print("DEBUG: Waiting for UDPProxy to close connections...")
        start_time = time.time()

        while time.time() - start_time < timeout:
            stdout, stderr = test_server.get_new_output_since_last_check()
            output = stdout + stderr

            if "Closed connection" in output:
                print("DEBUG: ✅ UDPProxy connections properly closed")
                return True

            time.sleep(0.1)  # Check every 100ms

        print("DEBUG: ⚠️  Timeout waiting for connection closure")
        return False

    def wait_for_connection_user(self, test_server, timeout=10):
        """Wait for UDPProxy to log 'User connection established' 
        indicating proper user connection."""
        print("DEBUG: Waiting for UDPProxy user connection...")
        start_time = time.time()

        while time.time() - start_time < timeout:
            stdout, stderr = test_server.get_new_output_since_last_check()
            output = stdout + stderr

            if "conn1" in output:
                print("DEBUG: ✅ User connection established")
                return True

            time.sleep(0.1)
        print("DEBUG: ⚠️  Timeout waiting for user connection")
        return False

    def print_udp_proxy_output(self, test_server):
        """Print the current output of the UDPProxy process for debugging."""
        stdout, stderr = test_server.get_new_output_since_last_check()
        all_output = stdout + stderr

        print(f"DEBUG: UDPProxy output: {all_output}")
        return all_output

    def check_udpproxy_output(self, test_server, expected_messages, num_lines=5):
        """Check UDPProxy stdout/stderr for expected messages.

        This method looks at the latest output lines to avoid the issue
        where get_new_output_since_last_check() returns empty because
        output was already consumed by previous calls.
        """
        stdout, stderr = test_server.get_latest_output(num_lines=num_lines)
        all_output = stdout + stderr

        print(
            f"DEBUG: Latest UDPProxy output (last {num_lines} lines): {all_output}")

        found_messages = []
        for expected in expected_messages:
            if expected in all_output:
                found_messages.append(expected)
                print(f"✅ Found expected message: '{expected}'")
            else:
                print(f"❌ Missing expected message: '{expected}'")

        return found_messages

    def create_connection(self, connection_type, port, source_system=1,
                          source_component=1):
        """Create a connection based on type (udp or tcp)."""
        if connection_type == 'udp':
            return mavutil.mavlink_connection(
                f'udpout:localhost:{port}',
                source_system=source_system,
                source_component=source_component,
                use_native=False
            )
        elif connection_type == 'tcp':
            return mavutil.mavlink_connection(
                f'tcp:localhost:{port}',
                source_system=source_system,
                source_component=source_component,
                autoreconnect=True,
                use_native=False
            )
        else:
            raise ValueError(f"Unknown connection type: {connection_type}")

    def setup_signing(self, connection, signing_key=None, enable_signing=True):
        """Setup signing for a connection."""
        if enable_signing and signing_key is not None:
            connection.setup_signing(signing_key, sign_outgoing=True)

    def create_message_sender(self, connection, message_types, stop_event):
        """Create a message sending function for a connection."""
        def sender():
            while not stop_event.is_set():
                for msg_type in message_types:
                    if msg_type == 'heartbeat_user':
                        connection.mav.heartbeat_send(
                            mavutil.mavlink.MAV_TYPE_QUADROTOR,
                            mavutil.mavlink.MAV_AUTOPILOT_ARDUPILOTMEGA,
                            0, 0, 0
                        )
                    elif msg_type == 'heartbeat_engineer':
                        connection.mav.heartbeat_send(
                            mavutil.mavlink.MAV_TYPE_GCS,
                            mavutil.mavlink.MAV_AUTOPILOT_INVALID,
                            0, 0, 0
                        )
                    elif msg_type == 'system_time':
                        current_time_us = int(time.time() * 1000000)
                        connection.mav.system_time_send(
                            current_time_us, 12345)
                time.sleep(1.0)
        return sender

    def run_test_scenario(self, test_server, user_conn_type, engineer_conn_type,
                          engineer_signing_key=None, test_duration=3):
        """Run a complete test scenario with message exchange."""
        user_conn = None
        engineer_conn = None

        try:
            port1, port2 = TEST_PORTS

            # Create connections
            user_conn = self.create_connection(user_conn_type, port1,
                                               source_system=1)
            engineer_conn = self.create_connection(engineer_conn_type, port2,
                                                   source_system=2)

            # Setup signing for engineer if provided
            self.setup_signing(engineer_conn, engineer_signing_key,
                               enable_signing=(engineer_signing_key is not None))

            # Start continuous message sending
            stop_sending = threading.Event()

            user_sender = self.create_message_sender(
                user_conn, ['heartbeat_user', 'system_time'], stop_sending)
            engineer_sender = self.create_message_sender(
                engineer_conn, ['heartbeat_engineer'], stop_sending)

            user_thread = threading.Thread(target=user_sender)
            engineer_thread = threading.Thread(target=engineer_sender)

            user_thread.start()
            self.wait_for_connection_user(test_server)
            engineer_thread.start()

            # Test for specified duration
            heartbeat_count = 0
            system_time_count = 0

            for i in range(test_duration):
                time.sleep(1.0)
                print(f"  Checking messages at second {i+1}...")

                # Check for HEARTBEAT
                heartbeat_msg = engineer_conn.recv_match(
                    type='HEARTBEAT', blocking=False)
                if heartbeat_msg is not None:
                    heartbeat_count += 1
                    print(f"Engineer received HEARTBEAT from system "
                          f"{heartbeat_msg.get_srcSystem()}")

                # Check for SYSTEM_TIME
                system_time_msg = engineer_conn.recv_match(
                    type='SYSTEM_TIME', blocking=False)
                if system_time_msg is not None:
                    system_time_count += 1
                    print(f"Engineer received SYSTEM_TIME: "
                          f"{system_time_msg.time_boot_ms}")

            # Stop sending
            stop_sending.set()
            user_thread.join()
            engineer_thread.join()

            return heartbeat_count, system_time_count

        finally:
            if user_conn:
                user_conn.close()
            if engineer_conn:
                engineer_conn.close()
            # Wait for UDPProxy to close connections before next test
            self.wait_for_connection_close(test_server)


class TestUDPConnections(BaseConnectionTest):
    """Test suite for UDP connection functionality using PyMAVLink 
    with authentication."""

    def test_basic_udp_message_reception(self, test_server):
        """Test UDP message reception with different authentication scenarios."""

        # Test scenario 1: Engineer without signed connection
        print("\n=== TEST 1: Engineer without signed connection ===")
        self._test_unsigned_engineer(test_server)

        # Test scenario 2: Engineer with bad signing key
        print("\n=== TEST 2: Engineer with bad signing key ===")
        self._test_bad_signing_key(test_server)

        # Test scenario 3: Engineer with correct signing key
        print("\n=== TEST 3: Engineer with correct signing key ===")
        self._test_good_signing_key(test_server)

    def _test_unsigned_engineer(self, test_server):
        """Test engineer connection without signing - should only get HEARTBEAT."""
        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, 'udp', 'udp', engineer_signing_key=None, test_duration=3
        )

        assert heartbeat_count > 0, \
            "Engineer should receive HEARTBEAT messages even without signing"
        assert system_time_count == 0, \
            "Engineer should NOT receive SYSTEM_TIME without proper signing"
        print(f"SUCCESS: Unsigned engineer received {heartbeat_count} HEARTBEAT, "
              f"{system_time_count} SYSTEM_TIME (expected)")

        expected_messages = ["Need to use support signing key"]
        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if "Need to use support signing key" in found_messages:
            print("✅ CONFIRMED: UDPProxy logged 'Need to use support signing key'")
        else:
            print("⚠️  UDPProxy output doesn't contain expected signing message")

    def _test_bad_signing_key(self, test_server):
        """Test engineer connection with bad signing key."""
        bad_passphrase = "wrong_auth"  # Different from TEST_PASSPHRASE
        bad_key = passphrase_to_key(bad_passphrase)

        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, 'udp', 'udp', engineer_signing_key=bad_key, test_duration=6
        )

        print(f"SUCCESS: Bad key engineer received {heartbeat_count} HEARTBEAT, "
              f"{system_time_count} SYSTEM_TIME (sent 6 seconds of signed messages)")

        expected_messages = ["Bad support signing key"]
        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if found_messages:
            print(f"✅ FOUND bad signing messages: {found_messages}")
        else:
            print("⚠️  UDPProxy output doesn't contain expected bad key message")

    def _test_good_signing_key(self, test_server):
        """Test engineer connection with correct signing key - should get both 
        HEARTBEAT and SYSTEM_TIME."""
        correct_key = passphrase_to_key(TEST_PASSPHRASE)

        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, 'udp', 'udp', engineer_signing_key=correct_key, test_duration=4
        )

        assert heartbeat_count > 0, "Engineer should receive HEARTBEAT messages"
        assert system_time_count > 0, \
            "Engineer should receive SYSTEM_TIME messages with correct signing"
        print(f"SUCCESS: Correct key engineer received {heartbeat_count} HEARTBEAT, "
              f"{system_time_count} SYSTEM_TIME")

        expected_messages = ["Got good signature"]
        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if found_messages:
            print(f"✅ FOUND authentication messages: {found_messages}")
        else:
            print("⚠️  No authentication messages found in UDPProxy output")


class TestTCPConnections(BaseConnectionTest):
    """Test suite for TCP connection functionality using PyMAVLink 
    with authentication."""

    def test_basic_tcp_message_reception(self, test_server):
        """Test TCP message reception with different authentication scenarios."""

        # Test scenario 1: Engineer without signed connection
        print("\n=== TCP TEST 1: Engineer without signed connection ===")
        self._test_unsigned_engineer_tcp(test_server)

        # Test scenario 2: Engineer with bad signing key
        print("\n=== TCP TEST 2: Engineer with bad signing key ===")
        self._test_bad_signing_key_tcp(test_server)

        # Test scenario 3: Engineer with correct signing key
        print("\n=== TCP TEST 3: Engineer with correct signing key ===")
        self._test_good_signing_key_tcp(test_server)

    def _test_unsigned_engineer_tcp(self, test_server):
        """Test engineer TCP connection without signing - should only get HEARTBEAT."""
        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, 'tcp', 'tcp', engineer_signing_key=None,
            test_duration=3
        )

        assert heartbeat_count > 0, \
            "TCP Engineer should receive HEARTBEAT messages even without signing"
        assert system_time_count == 0, \
            "TCP Engineer should NOT receive SYSTEM_TIME without proper signing"
        print(f"SUCCESS: TCP Unsigned engineer received {heartbeat_count} "
              f"HEARTBEAT, {system_time_count} SYSTEM_TIME (expected)")

        expected_messages = ["Need to use support signing key"]
        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if "Need to use support signing key" in found_messages:
            print("✅ CONFIRMED: UDPProxy logged 'Need to use support signing key' "
                  "for TCP")
        else:
            print("⚠️  UDPProxy output doesn't contain expected signing message "
                  "for TCP")

    def _test_bad_signing_key_tcp(self, test_server):
        """Test engineer TCP connection with bad signing key."""
        bad_passphrase = "wrong_auth"  # Different from TEST_PASSPHRASE
        bad_key = passphrase_to_key(bad_passphrase)

        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, 'tcp', 'tcp', engineer_signing_key=bad_key,
            test_duration=8
        )

        print(f"SUCCESS: TCP Bad key engineer received {heartbeat_count} "
              f"HEARTBEAT, {system_time_count} SYSTEM_TIME "
              f"(sent 8 seconds of signed messages)")

        expected_messages = ["Bad support signing key",
                             "Need to use support signing key"]
        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if found_messages:
            print(f"✅ FOUND TCP bad signing messages: {found_messages}")
        else:
            print("⚠️  UDPProxy output doesn't contain expected bad key message "
                  "for TCP")

    def _test_good_signing_key_tcp(self, test_server):
        """Test engineer TCP connection with correct signing key - should get 
        both HEARTBEAT and SYSTEM_TIME."""
        correct_key = passphrase_to_key(TEST_PASSPHRASE)

        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, 'tcp', 'tcp', engineer_signing_key=correct_key,
            test_duration=4
        )

        assert heartbeat_count > 0, "TCP Engineer should receive HEARTBEAT messages"

        assert system_time_count > 0, \
            "TCP Engineer should receive SYSTEM_TIME messages with correct signing"

        print(f"SUCCESS: TCP Correct key engineer received {heartbeat_count} "
              f"HEARTBEAT, {system_time_count} SYSTEM_TIME")

        expected_messages = ["Got good signature"]
        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if found_messages:
            print(f"✅ FOUND TCP authentication messages: {found_messages}")
        else:
            print("⚠️  No TCP authentication messages found in UDPProxy output")


class TestMixedConnections(BaseConnectionTest):
    """Test suite for mixed UDP/TCP connection scenarios."""

    def test_udp_user_tcp_engineer(self, test_server):
        """Test UDP user with TCP engineer connection."""
        print("\n=== MIXED TEST: UDP User + TCP Engineer ===")
        self._test_mixed_udp_user_tcp_engineer(test_server)

    def test_tcp_user_udp_engineer(self, test_server):
        """Test TCP user with UDP engineer connection."""
        print("\n=== MIXED TEST: TCP User + UDP Engineer ===")
        self._test_mixed_tcp_user_udp_engineer(test_server)

    def _test_mixed_udp_user_tcp_engineer(self, test_server):
        """Test UDP user connection with TCP engineer connection."""
        correct_key = passphrase_to_key(TEST_PASSPHRASE)

        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, 'udp', 'tcp', engineer_signing_key=correct_key,
            test_duration=4
        )

        assert heartbeat_count > 0, \
            "TCP Engineer should receive HEARTBEAT messages from UDP User"

        assert system_time_count > 0, \
            "TCP Engineer should receive SYSTEM_TIME messages from UDP User"

        print(f"SUCCESS: MIXED UDP/TCP - TCP Engineer received {heartbeat_count} "
              f"HEARTBEAT, {system_time_count} SYSTEM_TIME from UDP User")

        time.sleep(0.5)
        expected_messages = ["Got good signature"]
        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if found_messages:
            print(f"✅ FOUND mixed connection messages: {found_messages}")
        else:
            print("⚠️  Mixed connection setup may not be detected in logs")

    def _test_mixed_tcp_user_udp_engineer(self, test_server):
        """Test TCP user connection with UDP engineer connection."""
        correct_key = passphrase_to_key(TEST_PASSPHRASE)

        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, 'tcp', 'udp', engineer_signing_key=correct_key,
            test_duration=4
        )

        assert heartbeat_count > 0, \
            "UDP Engineer should receive HEARTBEAT messages from TCP User"

        assert system_time_count > 0, \
            "UDP Engineer should receive SYSTEM_TIME messages from TCP User"

        print(f"SUCCESS: MIXED TCP/UDP - UDP Engineer received {heartbeat_count} "
              f"HEARTBEAT, {system_time_count} SYSTEM_TIME from TCP User")

        time.sleep(0.5)
        expected_messages = ["Got good signature"]
        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if found_messages:
            print(f"✅ FOUND mixed connection messages: {found_messages}")
        else:
            print("⚠️  Mixed connection setup may not be detected in logs")


class TestTCPMultipleConnections(BaseConnectionTest):
    """Test suite for multiple TCP engineer connections capability."""

    def test_eight_tcp_engineer_connections(self, test_server):
        """Test that UDPProxy can handle 8 simultaneous TCP engineer 
        connections."""
        print("\n=== MULTIPLE TCP TEST: 8 TCP Engineer Connections ===")
        self._test_multiple_tcp_engineers(test_server)

    def _test_multiple_tcp_engineers(self, test_server):
        """Test 8 concurrent TCP engineer connections with authentication."""
        correct_key = passphrase_to_key(TEST_PASSPHRASE)
        user_conn = None
        engineer_connections = []

        try:
            port1, port2 = TEST_PORTS

            user_conn = self.create_connection('udp', port1, source_system=1)

            stop_sending = threading.Event()
            user_sender = self.create_message_sender(
                user_conn, ['heartbeat_user', 'system_time'], stop_sending)
            user_thread = threading.Thread(target=user_sender)
            user_thread.start()
            self.wait_for_connection_user(test_server)

            print(
                f"Creating {MAX_TCP_ENGINEER_CONNECTIONS} TCP engineer connections...")

            test_duration = MULTIPLE_CONNECTIONS_TEST_DURATION
            total_heartbeats = [0] * MAX_TCP_ENGINEER_CONNECTIONS
            total_system_times = [0] * MAX_TCP_ENGINEER_CONNECTIONS

            def check_engineer_messages(engineer_idx, engineer_conn):
                """Check messages for a specific engineer connection."""
                for second in range(test_duration):
                    time.sleep(1.0)
                    engineer_conn.mav.heartbeat_send(
                        mavutil.mavlink.MAV_TYPE_GCS,
                        mavutil.mavlink.MAV_AUTOPILOT_INVALID,
                        0, 0, 0
                    )
                    heartbeat_msg = engineer_conn.recv_match(
                        type='HEARTBEAT', blocking=False)
                    if heartbeat_msg is not None:
                        total_heartbeats[engineer_idx] += 1
                        print(f"Engineer {engineer_idx+1} received HEARTBEAT")

                    system_time_msg = engineer_conn.recv_match(
                        type='SYSTEM_TIME', blocking=False)
                    if system_time_msg is not None:
                        total_system_times[engineer_idx] += 1
                        print(
                            f"Engineer {engineer_idx+1} received SYSTEM_TIME")

            engineer_threads = []

            for i in range(MAX_TCP_ENGINEER_CONNECTIONS):
                engineer_conn = self.create_connection(
                    'tcp', port2, source_system=i+2)
                self.setup_signing(engineer_conn, correct_key,
                                   enable_signing=True)
                engineer_connections.append(engineer_conn)
                print(f"  Created engineer connection {i+1}/8")
                thread = threading.Thread(
                    target=check_engineer_messages,
                    args=(i, engineer_conn))
                engineer_threads.append(thread)
                thread.start()

            for thread in engineer_threads:
                thread.join()

            stop_sending.set()
            user_thread.join()

            all_received_heartbeats = all(
                count > 0 for count in total_heartbeats)
            all_received_system_times = all(
                count > 0 for count in total_system_times)

            print(
                f"\n=== RESULTS for {MAX_TCP_ENGINEER_CONNECTIONS} TCP Engineers ===")
            for i in range(MAX_TCP_ENGINEER_CONNECTIONS):
                print(f"Engineer {i+1}: {total_heartbeats[i]} HEARTBEAT, "
                      f"{total_system_times[i]} SYSTEM_TIME")

            assert all_received_heartbeats, \
                "All TCP engineers should receive HEARTBEAT messages"

            assert all_received_system_times, \
                "TCP engineers did not receive SYSTEM_TIME messages "

            print(f"✅ SUCCESS: All {MAX_TCP_ENGINEER_CONNECTIONS} TCP engineers "
                  f"successfully connected and received HEARTBEAT and SYSTEM_TIME messages")

            # Check UDPProxy output for good signatures
            time.sleep(0.5)
            expected_messages = ["Got good signature"]
            found_messages = self.check_udpproxy_output(test_server,
                                                        expected_messages, num_lines=15)

            if found_messages:
                print(f"✅ FOUND authentication messages: {found_messages}")
            else:
                print("⚠️  Multiple connection authentication not detected")

        finally:
            # Clean up all connections
            if user_conn:
                user_conn.close()
            for i, engineer_conn in enumerate(engineer_connections):
                try:
                    engineer_conn.close()
                    print(f"  Closed engineer connection {i+1}")
                except Exception:
                    pass

            # Wait for UDPProxy to close connections
            self.wait_for_connection_close(test_server)


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
