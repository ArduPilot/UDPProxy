"""
Test configuration and shared utilities for UDPProxy testing.
"""
import subprocess
import threading
import time
import pytest
import os
import shutil
import tempfile
from test_config import TEST_PORT_USER, TEST_PORT_ENGINEER, TEST_PASSPHRASE

os.environ['MAVLINK_DIALECT'] = 'ardupilotmega'
os.environ['MAVLINK20'] = '1'  # Ensure MAVLink2 is used


def create_temp_test_environment():
    """Create a temporary directory with all necessary test files."""
    temp_dir = tempfile.mkdtemp(prefix='udpproxy_test_')
    print(f"DEBUG: Created temporary test directory: {temp_dir}")

    # Copy necessary files to temp directory
    source_files = [
        '../udpproxy',  # Compiled binary
        '../keydb.py',  # Database management script
    ]

    for source_file in source_files:
        source_path = os.path.join(os.path.dirname(__file__), source_file)
        if os.path.exists(source_path):
            dest_path = os.path.join(temp_dir, os.path.basename(source_file))
            shutil.copy2(source_path, dest_path)
            # Make udpproxy executable
            if source_file.endswith('udpproxy'):
                os.chmod(dest_path, 0o755)
            print(f"DEBUG: Copied {source_file} to temp directory")
        else:
            print(f"WARNING: Source file {source_path} not found")

    # Create SSL certificates in temp directory
    cert_files = create_ssl_certificates(temp_dir)

    return temp_dir, cert_files


def create_ssl_certificates(temp_dir):
    """Create SSL certificates for WSS testing in the temp directory."""
    cert_files = {
        'fullchain': os.path.join(temp_dir, 'fullchain.pem'),
        'privkey': os.path.join(temp_dir, 'privkey.pem')
    }

    try:
        # Generate private key
        subprocess.run([
            'openssl', 'genrsa', '-out', cert_files['privkey'], '2048'
        ], check=True, capture_output=True)

        # Generate self-signed certificate
        subprocess.run([
            'openssl', 'req', '-new', '-x509',
            '-key', cert_files['privkey'],
            '-out', cert_files['fullchain'],
            '-days', '365',
            '-subj', '/C=US/ST=Test/L=Test/O=Test/CN=localhost',
            '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'
        ], check=True, capture_output=True)

        print("DEBUG: SSL certificates created in temp directory")
        return cert_files

    except subprocess.CalledProcessError as e:
        print(f"WARNING: Failed to create SSL certificates: {e}")
        return None


def cleanup_temp_environment(temp_dir):
    """Clean up the temporary test environment."""
    try:
        shutil.rmtree(temp_dir)
        print(f"DEBUG: Cleaned up temporary directory: {temp_dir}")
    except Exception as e:
        print(f"WARNING: Failed to cleanup temp directory {temp_dir}: {e}")


class UDPProxyProcess:
    def __init__(self, temp_dir, executable="udpproxy"):
        self.temp_dir = temp_dir
        self.executable_path = os.path.join(temp_dir, executable)
        self.last_output_time = time.time()
        self.last_seen_lines = set()

        # Start UDPProxy from the temporary directory
        self._start_process()

    def _start_process(self):
        """Start or restart the UDPProxy process."""
        self.proc = subprocess.Popen(
            [self.executable_path],
            cwd=self.temp_dir,  # Run from temp directory
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=1,
            universal_newlines=True
        )
        self._stdout_lines = []
        self._stderr_lines = []
        self._stdout_thread = threading.Thread(
            target=self._read_stream,
            args=(self.proc.stdout, self._stdout_lines))
        self._stderr_thread = threading.Thread(
            target=self._read_stream,
            args=(self.proc.stderr, self._stderr_lines))
        self._stdout_thread.daemon = True
        self._stderr_thread.daemon = True
        self._stdout_thread.start()
        self._stderr_thread.start()
        self._stdout_last_idx = 0
        self._stderr_last_idx = 0
        self.last_output_time = time.time()
        self.last_seen_lines = set()

    def _read_stream(self, stream, lines):
        for line in iter(stream.readline, ''):
            lines.append(line)
            self.last_output_time = time.time()
        stream.close()

    def is_responsive(self, max_age_seconds=30, max_repeated_lines=10):
        """Check if UDPProxy is responsive and not outputting stale data."""
        current_time = time.time()

        # Check if process is alive
        if self.proc.poll() is not None:
            exit_code = self.proc.poll()
            print(f"DEBUG: UDPProxy process has died (exit code: {exit_code})")
            return False

        # Check if output is too old (no new output for too long)
        if current_time - self.last_output_time > max_age_seconds:
            age = current_time - self.last_output_time
            print(f"DEBUG: UDPProxy output is stale ({age:.1f}s ago)")
            return False

        # Check for repeated lines (indicating stale output)
        recent_lines = []
        if len(self._stdout_lines) > 0:
            recent_lines.extend(self._stdout_lines[-5:])  # Last 5 stdout
        if len(self._stderr_lines) > 0:
            recent_lines.extend(self._stderr_lines[-5:])  # Last 5 stderr

        # Count repeated lines
        repeated_count = 0
        for line in recent_lines:
            if line.strip() in self.last_seen_lines:
                repeated_count += 1
            else:
                self.last_seen_lines.add(line.strip())

        # Keep only recent lines to prevent memory buildup
        if len(self.last_seen_lines) > 50:
            # Keep only the most recent lines
            recent_line_set = set(line.strip() for line in recent_lines)
            self.last_seen_lines = recent_line_set

        if repeated_count >= max_repeated_lines:
            print(f"DEBUG: UDPProxy has {repeated_count} repeated lines, "
                  f"appears stale")
            return False

        return True

    def restart_if_needed(self, force=False):
        """Restart UDPProxy if it's not responsive."""
        if not self.is_responsive() or force:
            print("DEBUG: UDPProxy appears unresponsive, restarting...")
            self.terminate()
            time.sleep(1)  # Wait for cleanup
            self._start_process()

            # Wait for restart to complete
            max_wait = 10
            start_time = time.time()
            while time.time() - start_time < max_wait:
                time.sleep(0.5)
                stdout, stderr = self.get_new_output_since_last_check()
                output = stdout + stderr

                if ("Opening sockets" in output and
                        "Added port 14552/14553" in output):
                    print("DEBUG: UDPProxy restarted successfully")
                    return True

            print("WARNING: UDPProxy restart may not have completed properly")
            return False
        return True

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

    # Create temporary test environment
    temp_dir, cert_files = create_temp_test_environment()
    server = None
    keydb_path = os.path.join(temp_dir, 'keydb.py')
    port1, port2 = TEST_PORT_USER, TEST_PORT_ENGINEER

    try:
        # Setup database in temp directory
        print("DEBUG: Setting up database entries in temp directory...")
        test_passphrase = TEST_PASSPHRASE

        # Initialize the database first
        result = subprocess.run(['python', keydb_path, 'initialise'],
                                cwd=temp_dir, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"WARNING: Database initialization: {result.stdout}")

        # Remove any existing entry first
        subprocess.run(['python', keydb_path, 'remove', str(port2)],
                       cwd=temp_dir, capture_output=True)

        # Add our test entry
        result = subprocess.run([
            'python', keydb_path, 'add', str(port1), str(port2),
            'test_user', test_passphrase
        ], cwd=temp_dir, capture_output=True, text=True)
        assert result.returncode == 0, (f"Failed to setup database: "
                                        f"{result.stderr}")

        # Verify database entry
        result = subprocess.run(['python', keydb_path, 'list'],
                                cwd=temp_dir, capture_output=True, text=True)
        print(f"DEBUG: Database contents in temp directory:\n"
              f"{result.stdout}")

        # Now start UDPProxy from temp directory
        print("DEBUG: Starting UDPProxy from temp directory...")
        server = UDPProxyProcess(temp_dir)

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
                print("DEBUG: UDPProxy loaded test port - ready!")
                break
        else:
            # Get all output for debugging
            stdout, stderr = server.get_new_output_since_last_check()
            output = stdout + stderr
            print(f"DEBUG: UDPProxy init timeout. Output:\n{output}")
            server.terminate()
            raise RuntimeError(
                "UDPProxy failed to initialize within timeout")

        print("DEBUG: test_server fixture setup complete")
        time.sleep(0.5)

        # Store temp_dir in server for cleanup
        server.temp_dir = temp_dir
        yield server

    finally:
        print("DEBUG: Tearing down test_server fixture")
        if server is not None:
            # Clean up database entry in temp directory
            subprocess.run(['python', keydb_path, 'remove', str(port2)],
                           cwd=temp_dir, capture_output=True)
            server.terminate()

        # Clean up temporary directory
        cleanup_temp_environment(temp_dir)
        print("DEBUG: test_server fixture teardown complete")


@pytest.fixture(scope="session")
def temp_test_env(test_server):
    """Fixture to provide access to temporary test environment paths."""
    return {
        'temp_dir': test_server.temp_dir,
        'ssl_cert': os.path.join(test_server.temp_dir, 'fullchain.pem'),
        'ssl_key': os.path.join(test_server.temp_dir, 'privkey.pem'),
        'keydb_path': os.path.join(test_server.temp_dir, 'keydb.py'),
        'udpproxy_path': os.path.join(test_server.temp_dir, 'udpproxy')
    }


@pytest.fixture(scope="function")
def healthy_server(test_server):
    """Fixture that ensures UDPProxy is healthy before each test."""
    print("DEBUG: Checking UDPProxy health before test")
    
    # Reset timestamps in database before each test to avoid timestamp issues
    reset_database_timestamps(test_server)

    # Check if server needs restart
    if not test_server.restart_if_needed():
        pytest.fail("UDPProxy failed to restart when needed")

    # Give it a moment to stabilize after restart
    time.sleep(0.5)

    print("DEBUG: UDPProxy health check passed")
    return test_server


def reset_database_timestamps(test_server):
    """Reset timestamps in the database to avoid timestamp-related issues."""
    keydb_path = os.path.join(test_server.temp_dir, 'keydb.py')
    port2 = TEST_PORT_ENGINEER
    
    try:
        print(f"DEBUG: Resetting timestamp for port {port2}")
        result = subprocess.run(
            ['python', keydb_path, 'resettimestamp', str(port2)],
            cwd=test_server.temp_dir, capture_output=True, text=True)
        if result.returncode == 0:
            print(f"DEBUG: Successfully reset timestamp: "
                  f"{result.stdout.strip()}")
        else:
            print(f"WARNING: Failed to reset timestamp: {result.stderr}")
    except Exception as e:
        print(f"WARNING: Exception resetting timestamp: {e}")
