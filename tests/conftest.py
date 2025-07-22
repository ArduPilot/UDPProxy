"""
Test configuration and shared utilities for UDPProxy testing.
"""
import subprocess
import threading
import time
import pytest
import os
from test_config import TEST_PORT_USER, TEST_PORT_ENGINEER, TEST_PASSPHRASE

os.environ['MAVLINK_DIALECT'] = 'ardupilotmega'
os.environ['MAVLINK20'] = '1'  # Ensure MAVLink2 is used


class UDPProxyProcess:
    def __init__(self, executable="./udpproxy"):
        self.proc = subprocess.Popen(
            [executable],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=1,
            universal_newlines=True
        )
        self._stdout_lines = []
        self._stderr_lines = []
        self._stdout_thread = threading.Thread(
            target=self._read_stream, args=(self.proc.stdout, self._stdout_lines))
        self._stderr_thread = threading.Thread(
            target=self._read_stream, args=(self.proc.stderr, self._stderr_lines))
        self._stdout_thread.daemon = True
        self._stderr_thread.daemon = True
        self._stdout_thread.start()
        self._stderr_thread.start()
        self._stdout_last_idx = 0
        self._stderr_last_idx = 0

    def _read_stream(self, stream, lines):
        for line in iter(stream.readline, ''):
            lines.append(line)
        stream.close()

    def get_new_output_since_last_check(self):
        stdout_new = self._stdout_lines[self._stdout_last_idx:]
        stderr_new = self._stderr_lines[self._stderr_last_idx:]
        self._stdout_last_idx = len(self._stdout_lines)
        self._stderr_last_idx = len(self._stderr_lines)
        return ''.join(stdout_new), ''.join(stderr_new)

    def get_latest_output(self, num_lines=5):
        """Get the latest N lines from stdout/stderr combined."""
        # Get the latest num_lines from both stdout and stderr
        if len(self._stdout_lines) >= num_lines:
            latest_stdout = self._stdout_lines[-num_lines:]
        else:
            latest_stdout = self._stdout_lines

        if len(self._stderr_lines) >= num_lines:
            latest_stderr = self._stderr_lines[-num_lines:]
        else:
            latest_stderr = self._stderr_lines

        # Combine and return as strings
        return ''.join(latest_stdout), ''.join(latest_stderr)

    def terminate(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


@pytest.fixture(scope="session")
def test_server():
    """Pytest fixture to provide a test server instance."""
    print("DEBUG: Setting up test_server fixture")

    # Setup database BEFORE starting UDPProxy
    print("DEBUG: Setting up database entries before starting UDPProxy...")
    port1, port2 = TEST_PORT_USER, TEST_PORT_ENGINEER
    test_passphrase = TEST_PASSPHRASE

    # Remove any existing entry first
    subprocess.run(['python', 'keydb.py', 'remove', str(port2)],
                   capture_output=True)

    # Add our test entry
    result = subprocess.run([
        'python', 'keydb.py', 'add', str(port1), str(port2),
        'test_user', test_passphrase
    ], capture_output=True, text=True)
    assert result.returncode == 0, f"Failed to setup database: {result.stderr}"

    # Verify database entry
    result = subprocess.run(['python', 'keydb.py', 'list'],
                            capture_output=True, text=True)
    print(
        f"DEBUG: Database contents before starting UDPProxy:\n{result.stdout}")

    # Now start UDPProxy with database already populated
    print("DEBUG: Starting UDPProxy with database ready...")
    server = UDPProxyProcess()

    # Wait for UDPProxy to be fully initialized
    print("DEBUG: Waiting for UDPProxy to initialize...")
    max_wait = 10  # Maximum wait time in seconds
    start_time = time.time()

    while time.time() - start_time < max_wait:
        time.sleep(0.5)
        stdout, stderr = server.get_new_output_since_last_check()
        output = stdout + stderr

        if "Opening sockets" in output:
            print("DEBUG: UDPProxy has started opening sockets")
        if "Added port 14552/14553" in output:
            print("DEBUG: UDPProxy has loaded our test port - ready for testing!")
            break
    else:
        # Get all output for debugging
        stdout, stderr = server.get_new_output_since_last_check()
        output = stdout + stderr
        print(
            f"DEBUG: UDPProxy initialization timeout. Output so far:\n{output}")
        server.terminate()
        raise RuntimeError(
            "UDPProxy failed to initialize properly within timeout")

    print("DEBUG: test_server fixture setup complete")
    yield server

    print("DEBUG: Tearing down test_server fixture")
    # Clean up database entry
    subprocess.run(['python', 'keydb.py', 'remove', str(port2)],
                   capture_output=True)
    server.terminate()
    print("DEBUG: test_server fixture teardown complete")
