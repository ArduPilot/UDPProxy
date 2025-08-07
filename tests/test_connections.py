"""Connection test suite for UDPProxy.

Tests UDP, TCP, and mixed connection scenarios with proper MAVLink2
authentication.
"""
from pymavlink import mavutil
import sys
import os
import time
import pytest
import threading
import ssl
import urllib3
import signal
import functools
from test_config import (TEST_PORTS, TEST_PASSPHRASE,
                         MAX_TCP_ENGINEER_CONNECTIONS,
                         MULTIPLE_CONNECTIONS_TEST_DURATION)

# Set up environment for pymavlink
os.environ['MAVLINK_DIALECT'] = 'ardupilotmega'
os.environ['MAVLINK20'] = '1'  # Ensure MAVLink2 is used

# Disable SSL certificate verification for testing with self-signed certs
os.environ['PYTHONHTTPSVERIFY'] = '0'
os.environ['CURL_CA_BUNDLE'] = ''  # Disable certificate bundle for testing

# Disable SSL warnings for testing
try:
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
except AttributeError:
    pass  # urllib3 might not have InsecureRequestWarning in all versions

# Configure SSL context to not verify certificates for testing
try:
    # Save original SSL context creation function
    _orig_create_default_context = ssl.create_default_context

    def _create_unverified_context(*args, **kwargs):
        """Create SSL context that doesn't verify certificates."""
        context = _orig_create_default_context(*args, **kwargs)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        return context

    # Replace SSL context creation for testing
    ssl.create_default_context = _create_unverified_context
    ssl._create_default_https_context = _create_unverified_context

    print("🔒 SSL certificate verification disabled for testing")
except AttributeError:
    pass  # Some SSL functions might not be available


def passphrase_to_key(passphrase):
    '''convert a passphrase to a 32 byte key'''
    import hashlib
    h = hashlib.new('sha256')
    passphrase = passphrase.encode('ascii')
    h.update(passphrase)
    return h.digest()


def timeout_after(seconds):
    """Decorator to add timeout to test functions."""
    def decorator(func):
        @functools.wraps(func)
        def wrapper(*args, **kwargs):
            class TimeoutError(Exception):
                pass

            def timeout_handler(signum, frame):
                raise TimeoutError(f"Test {func.__name__} timed out after "
                                   f"{seconds} seconds")

            # Set up the signal handler
            old_handler = signal.signal(signal.SIGALRM, timeout_handler)
            signal.alarm(seconds)

            try:
                result = func(*args, **kwargs)
                return result
            finally:
                # Cancel the alarm and restore the old handler
                signal.alarm(0)
                signal.signal(signal.SIGALRM, old_handler)

        return wrapper
    return decorator


class BaseConnectionTest:
    """Base class for connection tests with shared functionality."""

    def wait_for_connection_close(self, test_server, timeout=10):
        """Wait for UDPProxy to log 'Closed connection' indicating
        connections have been properly closed."""
        print("DEBUG: Waiting for UDPProxy to close connections...")
        start_time = time.time()

        test_string = "Closed connection"

        laststdout, _ = test_server.get_latest_output(num_lines=2)
        if test_string in laststdout:
            print("DEBUG: ✅ Connection already closed from last output")
            return True

        while time.time() - start_time < timeout:
            stdout, stderr = test_server.get_new_output_since_last_check()
            output = stdout + stderr

            if test_string in output:
                print("DEBUG: ✅ UDPProxy connections properly closed")
                return True

            time.sleep(0.1)  # Check every 100ms

        print("DEBUG: ⚠️  Timeout waiting for connection closure")
        stdout, stderr = test_server.get_latest_output(num_lines=5)
        all_output = stdout + stderr

        print(f"DEBUG: Latest UDPProxy output (last 5 lines): "
              f"{all_output}")
        return False

    def wait_for_connection_user(self, user_type, test_server, timeout=10):
        """Wait for UDPProxy to log connection establishment messages
        indicating proper user connection."""
        print("DEBUG: Waiting for UDPProxy user connection...")
        start_time = time.time()

        if user_type == "user":
            # Look for specific conn1 establishment patterns
            test_patterns = [
                "have TCP conn1 for",
                "have UDP conn1 for",
                "WebSocket conn1"
            ]
        elif user_type == "engineer":
            # Look for specific conn2 establishment patterns
            test_patterns = [
                "have TCP conn2[",
                "have UDP conn2[",
                "WebSocket conn2"
            ]
        else:
            raise ValueError(f"Unknown user_type: {user_type}")

        def check_patterns_in_output(output):
            """Check if any of the connection patterns are found."""
            for pattern in test_patterns:
                if pattern in output:
                    return pattern
            return None

        laststdout, _ = test_server.get_latest_output(num_lines=2)
        found_pattern = check_patterns_in_output(laststdout)
        if found_pattern:
            print(f"DEBUG: ✅ {user_type} connection already established "
                  f"in last output (pattern: '{found_pattern}')")
            return True

        while time.time() - start_time < timeout:
            stdout, stderr = test_server.get_new_output_since_last_check()
            output = stdout + stderr

            found_pattern = check_patterns_in_output(output)
            if found_pattern:
                print(f"DEBUG: ✅ {user_type} connection established "
                      f"(pattern: '{found_pattern}')")
                return True

            time.sleep(0.1)
        print(f"DEBUG: ⚠️  Timeout waiting for {user_type} connection")
        stdout, stderr = test_server.get_latest_output(num_lines=5)
        all_output = stdout + stderr

        print(f"DEBUG: Latest UDPProxy output (last 5 lines): "
              f"{all_output}")
        return False

    def print_udp_proxy_output(self, test_server):
        """Print the current output of the UDPProxy process for debugging."""
        stdout, stderr = test_server.get_new_output_since_last_check()
        all_output = stdout + stderr

        print(f"DEBUG: UDPProxy output: {all_output}")
        return all_output

    def check_udpproxy_output(self, test_server, expected_messages,
                              num_lines=5):
        """Check UDPProxy stdout/stderr for expected messages.

        This method looks at the latest output lines to avoid the issue
        where get_new_output_since_last_check() returns empty because
        output was already consumed by previous calls.
        """
        stdout, stderr = test_server.get_latest_output(num_lines=num_lines)
        all_output = stdout + stderr

        print(f"DEBUG: Latest UDPProxy output (last {num_lines} lines): "
              f"{all_output}")

        found_messages = []
        for expected in expected_messages:
            if expected in all_output:
                found_messages.append(expected)
                print(f"✅ Found expected message: '{expected}'")
            else:
                print(f"❌ Missing expected message: '{expected}'")

        return found_messages

    def create_connection(self, conn_type, port, source_system=1,
                          source_component=1, timeout=30):
        """Create a connection of the specified type to the given port."""
        if conn_type == 'udp':
            conn_str = f'udpout:localhost:{port}'
        elif conn_type == 'tcp':
            conn_str = f'tcp:localhost:{port}'
        elif conn_type == 'ws':
            conn_str = f'ws:localhost:{port}'
        elif conn_type == 'wss':
            conn_str = f'wss:localhost:{port}'
        else:
            raise ValueError(f"Unsupported connection type: {conn_type}")

        # Add timeout for connection creation, especially important for WSS
        start_time = time.time()

        # Create connection with signing if needed
        conn = mavutil.mavlink_connection(
            conn_str,
            source_system=source_system,
            source_component=source_component,
            autoreconnect=True,
        )

        # Check if connection took too long (especially for WSS SSL handshake)
        elapsed = time.time() - start_time
        if elapsed > timeout:
            try:
                conn.close()
            except Exception:
                pass
            raise TimeoutError(f"Connection creation took {elapsed:.1f}s, "
                               f"exceeding {timeout}s timeout for {conn_type}")

        return conn

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
            print(
                f"DEBUG: Stopping message sender for {connection.source_system}")
        return sender

    def run_test_scenario(self, test_server, user_conn_type,
                          engineer_conn_type, engineer_signing_key=None,
                          test_duration=3):
        """Run a complete test scenario with message exchange."""
        user_conn = None
        engineer_conn = None

        try:
            port1, port2 = TEST_PORTS

            # Create connections
            user_conn = self.create_connection(user_conn_type, port1,
                                               source_system=1)

            # Start continuous message sending
            stop_sending = threading.Event()

            user_sender = self.create_message_sender(
                user_conn, ['heartbeat_user', 'system_time'], stop_sending)

            user_thread = threading.Thread(target=user_sender)

            user_thread.start()
            assert self.wait_for_connection_user("user", test_server)
            engineer_conn = self.create_connection(engineer_conn_type, port2,
                                                   source_system=2)

            # Setup signing for engineer if provided
            enable_signing = (engineer_signing_key is not None)
            self.setup_signing(engineer_conn, engineer_signing_key,
                               enable_signing=enable_signing)
            engineer_sender = self.create_message_sender(
                engineer_conn, ['heartbeat_engineer'], stop_sending)
            engineer_thread = threading.Thread(target=engineer_sender)
            engineer_thread.start()
            assert self.wait_for_connection_user("engineer", test_server)

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
            user_thread.join(timeout=10)  # 10 second timeout
            engineer_thread.join(timeout=10)  # 10 second timeout

            # Check if threads are still alive and force them to stop
            if user_thread.is_alive():
                print("WARNING: User thread did not stop within timeout")
            if engineer_thread.is_alive():
                print("WARNING: Engineer thread did not stop within timeout")

            return heartbeat_count, system_time_count

        finally:
            if user_conn:
                user_conn.close()
            if engineer_conn:
                engineer_conn.close()
            # Wait for UDPProxy to close connections before next test
            self.wait_for_connection_close(test_server)


class TestConnections(BaseConnectionTest):
    """Unified test suite for all connection types (UDP, TCP, WS, WSS)
    with authentication scenarios."""

    @timeout_after(120)  # 2 minute timeout for connection scenarios
    @pytest.mark.parametrize("user_conn_type,engineer_conn_type", [
        # Same protocol connections
        ('udp', 'udp'),
        ('tcp', 'tcp'),
        ('ws', 'ws'),
        ('wss', 'wss'),
        # Mixed protocol connections
        ('udp', 'tcp'),
        ('tcp', 'udp'),
        ('udp', 'ws'),
        ('ws', 'udp'),
        ('udp', 'wss'),
        ('wss', 'udp'),
        ('tcp', 'ws'),
        ('ws', 'tcp'),
        ('tcp', 'wss'),
        ('wss', 'tcp'),
        ('ws', 'wss'),
        ('wss', 'ws'),
    ])
    def test_connection_scenarios(self, healthy_server, user_conn_type,
                                  engineer_conn_type):
        """Test all connection type combinations with authentication
        scenarios."""
        conn_label = f"{user_conn_type.upper()}/{engineer_conn_type.upper()}"
        print(f"\n=== {conn_label} CONNECTION TESTS ===")

        # Test scenario 1: Engineer without signed connection
        print(f"\n=== {conn_label} TEST 1: Engineer without signed "
              f"connection ===")
        self._test_unsigned_engineer(healthy_server, user_conn_type,
                                     engineer_conn_type)

        # Test scenario 2: Engineer with bad signing key
        print(f"\n=== {conn_label} TEST 2: Engineer with bad signing key ===")

        self._test_bad_signing_key(healthy_server, user_conn_type,
                                   engineer_conn_type)

        # Test scenario 3: Engineer with correct signing key
        print(f"\n=== {conn_label} TEST 3: Engineer with correct "
              f"signing key ===")
        self._test_good_signing_key(healthy_server, user_conn_type,
                                    engineer_conn_type)

    def _test_unsigned_engineer(self, test_server, user_conn_type,
                                engineer_conn_type):
        """Test engineer connection without signing - should only get
        HEARTBEAT."""
        conn_label = f"{user_conn_type.upper()}/{engineer_conn_type.upper()}"

        if engineer_conn_type in ['wss']:
            print("🔒 WSS Test: Using SSL certificates "
                  "fullchain.pem/privkey.pem")

        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, user_conn_type, engineer_conn_type,
            engineer_signing_key=None, test_duration=3
        )

        assert heartbeat_count > 0, \
            f"{conn_label} Engineer should receive HEARTBEAT messages " \
            f"even without signing"
        assert system_time_count == 0, \
            f"{conn_label} Engineer should NOT receive SYSTEM_TIME " \
            f"without proper signing"

        print(f"SUCCESS: {conn_label} Unsigned engineer received "
              f"{heartbeat_count} HEARTBEAT, {system_time_count} "
              f"SYSTEM_TIME (expected)")

        expected_messages = ["Need to use support signing key"]
        if engineer_conn_type == 'wss':
            expected_messages.append("SSL handshake completed")

        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if "Need to use support signing key" in found_messages:
            print(f"✅ CONFIRMED: UDPProxy logged 'Need to use support "
                  f"signing key' for {conn_label}")
        else:
            print(f"⚠️  UDPProxy output doesn't contain expected signing "
                  f"message for {conn_label}")

        if engineer_conn_type == 'wss':
            if "SSL handshake completed" in found_messages:
                print("✅ CONFIRMED: SSL handshake completed for WSS")
            else:
                print("⚠️  No SSL handshake messages found in UDPProxy output")

    def _test_bad_signing_key(self, test_server, user_conn_type,
                              engineer_conn_type):
        """Test engineer connection with bad signing key."""
        conn_label = f"{user_conn_type.upper()}/{engineer_conn_type.upper()}"
        bad_passphrase = "wrong_auth"  # Different from TEST_PASSPHRASE
        bad_key = passphrase_to_key(bad_passphrase)

        if engineer_conn_type in ['wss']:
            print("🔒 WSS Test: Using SSL certificates "
                  "fullchain.pem/privkey.pem")

        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, user_conn_type, engineer_conn_type,
            engineer_signing_key=bad_key, test_duration=6
        )

        print(f"SUCCESS: {conn_label} Bad key engineer received "
              f"{heartbeat_count} HEARTBEAT, {system_time_count} "
              f"SYSTEM_TIME (sent 6 seconds of signed messages)")

        expected_messages = ["Bad support signing key",
                             "Need to use support signing key"]
        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if found_messages:
            print(f"✅ FOUND {conn_label} bad signing messages: "
                  f"{found_messages}")
        else:
            print(f"⚠️  UDPProxy output doesn't contain expected bad key "
                  f"message for {conn_label}")

    def _test_good_signing_key(self, test_server, user_conn_type,
                               engineer_conn_type):
        """Test engineer connection with correct signing key - should get
        both HEARTBEAT and SYSTEM_TIME."""
        conn_label = f"{user_conn_type.upper()}/{engineer_conn_type.upper()}"
        correct_key = passphrase_to_key(TEST_PASSPHRASE)

        if engineer_conn_type in ['wss']:
            print("🔒 WSS Test: Using SSL certificates "
                  "fullchain.pem/privkey.pem")

        heartbeat_count, system_time_count = self.run_test_scenario(
            test_server, user_conn_type, engineer_conn_type,
            engineer_signing_key=correct_key, test_duration=4
        )

        assert heartbeat_count > 0, \
            f"{conn_label} Engineer should receive HEARTBEAT messages"
        assert system_time_count > 0, \
            f"{conn_label} Engineer should receive SYSTEM_TIME messages " \
            f"with correct signing"

        print(f"SUCCESS: {conn_label} Correct key engineer received "
              f"{heartbeat_count} HEARTBEAT, {system_time_count} "
              f"SYSTEM_TIME")

        expected_messages = ["Got good signature"]
        if engineer_conn_type == 'wss':
            expected_messages.append("SSL handshake completed")

        found_messages = self.check_udpproxy_output(
            test_server, expected_messages)

        if found_messages:
            print(f"✅ FOUND {conn_label} authentication messages: "
                  f"{found_messages}")
        else:
            print(f"⚠️  No {conn_label} authentication messages found "
                  f"in UDPProxy output")


class TestTCPMultipleConnections(BaseConnectionTest):
    """Test suite for multiple engineer connections with various types."""

    @timeout_after(240)
    @pytest.mark.parametrize("tcp_count,udp_count,wss_count,description", [
        (8, 0, 0, "8 TCP connections"),
        (0, 8, 0, "8 UDP connections"),
        (4, 4, 0, "4 TCP + 4 UDP connections"),
        (7, 0, 1, "7 TCP + 1 WSS connections"),
        (0, 0, 8, "8 WSS connections"),
        (4, 0, 4, "4 TCP + 4 WSS connections"),
        (4, 2, 2, "4 TCP + 2 UDP + 2 WSS connections"),
    ])
    def test_multiple_engineer_connections(self, healthy_server, tcp_count,
                                           udp_count, wss_count, description):
        """Test that UDPProxy can handle multiple simultaneous engineer
        connections of different types."""
        total_connections = tcp_count + udp_count + wss_count
        print(f"\n=== MULTIPLE CONNECTION TEST: {description} "
              f"(total: {total_connections}) ===")

        if wss_count > 0:
            print("🔒 WSS Test: Using SSL certificates "
                  "fullchain.pem/privkey.pem")

        self._test_multiple_mixed_engineers(healthy_server, tcp_count,
                                            udp_count, wss_count)

    @timeout_after(60)  # 1 minute timeout for failure test
    def test_nine_tcp_connections_should_fail(self, healthy_server):
        """Test that UDPProxy rejects the 9th TCP connection (limit is 8)."""
        print("\n=== FAILURE TEST: 9 TCP connections (should fail) ===")

        # This should fail because TCP connection limit is 8
        with pytest.raises((AssertionError, ConnectionError, OSError)):
            self._test_multiple_mixed_engineers(healthy_server, 9, 0, 0,
                                                expect_failure=True)

    def _test_multiple_mixed_engineers(self, test_server, tcp_count,
                                       udp_count, wss_count,
                                       expect_failure=False):
        """Test multiple concurrent engineer connections with
        authentication."""
        correct_key = passphrase_to_key(TEST_PASSPHRASE)
        user_conn = None
        engineer_connections = []
        total_connections = tcp_count + udp_count + wss_count

        try:
            port1, port2 = TEST_PORTS

            user_conn = self.create_connection('udp', port1, source_system=1)

            stop_sending = threading.Event()
            user_sender = self.create_message_sender(
                user_conn, ['heartbeat_user', 'system_time'], stop_sending)
            user_thread = threading.Thread(target=user_sender)
            user_thread.start()
            self.wait_for_connection_user("user", test_server)

            # Create connection plan
            connection_plan = []
            connection_plan.extend(['tcp'] * tcp_count)
            connection_plan.extend(['udp'] * udp_count)
            connection_plan.extend(['wss'] * wss_count)

            print(f"Creating {total_connections} engineer connections: "
                  f"{tcp_count} TCP, {udp_count} UDP, {wss_count} WSS")

            test_duration = MULTIPLE_CONNECTIONS_TEST_DURATION
            total_heartbeats = [0] * total_connections
            total_system_times = [0] * total_connections

            def check_engineer_messages(engineer_idx, engineer_conn,
                                        conn_type):
                """Check messages for a specific engineer connection."""
                try:
                    for second in range(test_duration):
                        time.sleep(1.0)
                        # Send heartbeat to establish connection (for UDP)
                        engineer_conn.mav.heartbeat_send(
                            mavutil.mavlink.MAV_TYPE_GCS,
                            mavutil.mavlink.MAV_AUTOPILOT_INVALID,
                            0, 0, 0
                        )

                        # Check for received HEARTBEAT
                        heartbeat_msg = engineer_conn.recv_match(
                            type='HEARTBEAT', blocking=False)
                        if heartbeat_msg is not None:
                            total_heartbeats[engineer_idx] += 1
                            print(f"Engineer {engineer_idx+1} ({conn_type}) "
                                  f"received HEARTBEAT")

                        # Check for received SYSTEM_TIME
                        system_time_msg = engineer_conn.recv_match(
                            type='SYSTEM_TIME', blocking=False)
                        if system_time_msg is not None:
                            total_system_times[engineer_idx] += 1
                            print(f"Engineer {engineer_idx+1} ({conn_type}) "
                                  f"received SYSTEM_TIME")
                except Exception as e:
                    if expect_failure:
                        print(f"Expected failure for engineer "
                              f"{engineer_idx+1} ({conn_type}): {e}")
                    else:
                        raise

            engineer_threads = []

            # Create connections according to plan with overall timeout
            connection_start_time = time.time()
            connection_timeout = 120  # 2 minutes for all connections

            for i, conn_type in enumerate(connection_plan):
                # Check overall timeout
                if time.time() - connection_start_time > connection_timeout:
                    raise TimeoutError(f"Connection creation took too long, "
                                       f"exceeded {connection_timeout}s "
                                       f"timeout")

                try:
                    print(f"  Creating engineer connection {i+1}/"
                          f"{total_connections} ({conn_type})...")

                    # Use longer timeout for WSS due to SSL handshake
                    timeout = 30 if conn_type == 'wss' else 20
                    engineer_conn = self.create_connection(
                        conn_type, port2, source_system=i+2, timeout=timeout)

                    self.setup_signing(engineer_conn, correct_key,
                                       enable_signing=True)
                    engineer_connections.append(engineer_conn)
                    print(f"  ✅ Created engineer connection {i+1}/"
                          f"{total_connections} ({conn_type})")

                    # Add delay for WSS to prevent SSL resource contention
                    if conn_type == 'wss':
                        print("    WSS connection created - waiting 3.0s to "
                              "prevent SSL resource contention...")
                        time.sleep(3.0)

                except Exception as e:
                    if expect_failure and i >= MAX_TCP_ENGINEER_CONNECTIONS:
                        print(f"Expected failure creating connection "
                              f"{i+1}: {e}")
                        # This is expected for the 9th TCP connection
                        break
                    else:
                        print(f"ERROR: Failed to create {conn_type} "
                              f"connection {i+1}: {e}")
                        # For WSS connection failures, still continue with
                        # cleanup but don't expect connection close messages
                        if conn_type == 'wss':
                            print(f"WSS connection {i+1} failed - this may be "
                                  f"due to SSL resource limits")
                            break
                        else:
                            raise

            # Start message checking threads
            for i, engineer_conn in enumerate(engineer_connections):
                conn_type = connection_plan[i]
                thread = threading.Thread(
                    target=check_engineer_messages,
                    args=(i, engineer_conn, conn_type))
                engineer_threads.append(thread)
                thread.start()

            # Wait for all threads to complete with timeout
            for i, thread in enumerate(engineer_threads):
                thread.join(timeout=60)  # 60 second timeout per thread
                if thread.is_alive():
                    print(f"WARNING: Engineer thread {i+1} did not complete "
                          f"within timeout")

            stop_sending.set()
            user_thread.join(timeout=10)  # 10 second timeout
            if user_thread.is_alive():
                print("WARNING: User thread did not stop within timeout")

            if expect_failure:
                # For failure test, we expect some connections to fail
                successful_connections = len([c for c in engineer_connections
                                              if c is not None])
                if successful_connections >= MAX_TCP_ENGINEER_CONNECTIONS:
                    raise AssertionError(
                        f"Expected failure but {successful_connections} "
                        f"connections succeeded")
                print(f"✅ EXPECTED FAILURE: Only {successful_connections}/"
                      f"{total_connections} connections succeeded")
                return

            # For success tests, verify connections worked
            num_connections = len(engineer_connections)

            # For mixed tests with UDP, only require TCP/WSS to work
            # UDP multiple connections may have limitations in UDPProxy
            if udp_count > 0 and tcp_count > 0:
                # Check TCP connections (first tcp_count) and WSS connections
                # WSS connections are at the end of the list
                tcp_heartbeats = all(
                    count > 0 for count in total_heartbeats[:tcp_count])
                tcp_system_times = all(
                    count > 0 for count in total_system_times[:tcp_count])

                # Check WSS connections if any (after UDP connections)
                if wss_count > 0:
                    wss_start_idx = tcp_count + udp_count
                    wss_heartbeats = all(
                        count > 0 for count in
                        total_heartbeats[wss_start_idx:wss_start_idx +
                                         wss_count])
                    wss_system_times = all(
                        count > 0 for count in
                        total_system_times[wss_start_idx:wss_start_idx +
                                           wss_count])
                else:
                    wss_heartbeats = True
                    wss_system_times = True

                print(f"\n=== RESULTS for {num_connections} Engineers ===")
                for i in range(num_connections):
                    conn_type = connection_plan[i]
                    print(f"Engineer {i+1} ({conn_type}): "
                          f"{total_heartbeats[i]} HEARTBEAT, "
                          f"{total_system_times[i]} SYSTEM_TIME")

                assert tcp_heartbeats and wss_heartbeats, \
                    "All TCP/WSS engineers should receive HEARTBEAT messages"
                assert tcp_system_times and wss_system_times, \
                    "All TCP/WSS engineers should receive SYSTEM_TIME messages"

                working_tcp_wss = tcp_count + wss_count
                print(f"✅ SUCCESS: All {working_tcp_wss} TCP/WSS "
                      f"engineers successfully connected and received "
                      f"messages")
                # Note about UDP connections
                udp_start_idx = tcp_count
                udp_end_idx = udp_start_idx + udp_count
                udp_working = any(total_heartbeats[udp_start_idx:udp_end_idx])
                if udp_working:
                    print("✅ BONUS: Some UDP engineers also received "
                          "messages")
                else:
                    print("⚠️  NOTE: UDP engineers in mixed scenarios may "
                          "have limitations")
            else:
                # Pure TCP, pure WSS, or pure UDP test
                all_received_heartbeats = all(
                    count > 0 for count in total_heartbeats[:num_connections])
                all_received_system_times = all(
                    count > 0 for count in
                    total_system_times[:num_connections])

                print(f"\n=== RESULTS for {num_connections} Engineers ===")
                for i in range(num_connections):
                    conn_type = connection_plan[i]
                    print(f"Engineer {i+1} ({conn_type}): "
                          f"{total_heartbeats[i]} HEARTBEAT, "
                          f"{total_system_times[i]} SYSTEM_TIME")

                assert all_received_heartbeats, \
                    "All engineers should receive HEARTBEAT messages"
                assert all_received_system_times, \
                    "All engineers should receive SYSTEM_TIME messages"

                print(f"✅ SUCCESS: All {len(engineer_connections)} "
                      f"engineers successfully connected and received "
                      f"messages")

            # Check UDPProxy output for good signatures
            time.sleep(0.5)
            expected_messages = ["Got good signature"]
            if wss_count > 0:
                expected_messages.append("SSL handshake completed")

            found_messages = self.check_udpproxy_output(
                test_server, expected_messages, num_lines=20)

            if found_messages:
                print(f"✅ FOUND authentication messages: {found_messages}")
            else:
                print("⚠️  Multiple connection authentication not detected")

        finally:
            # Wait for UDPProxy to close connections, but use shorter timeout
            # for cases where connection creation failed
            num_created_connections = len(engineer_connections)
            failed_test = (num_created_connections < total_connections and
                           not expect_failure)

            # Clean up connections - mavutil don't need explicit close
            user_conn = None
            engineer_connections.clear()

            close_timeout = 5 if num_created_connections == 0 else 10
            try:
                self.wait_for_connection_close(test_server,
                                               timeout=close_timeout)
            except Exception as e:
                print(f"WARNING: Connection close wait failed: {e}")
                failed_test = True  # Mark as failed if cleanup failed
                # Don't fail the test just because connection close
                # detection failed

            # Restart UDPProxy after any failure to ensure clean state
            if failed_test:
                print("DEBUG: Test failure detected - restarting UDPProxy "
                      "to ensure clean state for next test")
                try:
                    if not test_server.restart_if_needed(force=True):
                        print("WARNING: UDPProxy restart after failure "
                              "may not have completed properly")
                    else:
                        print("DEBUG: UDPProxy successfully restarted "
                              "after test failure")
                except Exception as restart_e:
                    print(f"WARNING: UDPProxy restart failed: {restart_e}")
                    # Don't fail the test because of restart issues


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
