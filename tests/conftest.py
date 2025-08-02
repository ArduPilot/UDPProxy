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

        # Start UDPProxy from the temporary directory
        self.proc = subprocess.Popen(
            [self.executable_path],
            cwd=temp_dir,  # Run from temp directory
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
