#!/bin/bash

# Test runner script for UDPProxy
# This script runs all tests with proper error handling

set -e  # Exit on any error
set -u  # Exit on undefined variable
set -o pipefail  # Exit on pipe failure

# Ensure we're running from repository root
cd "$(dirname "$0")/.."

echo "=== UDPProxy Test Runner ==="

# Cleanup function to restore database on exit
cleanup() {
    echo ""
    echo "=== Cleanup ==="

    # Kill any remaining UDPProxy processes
    pkill -f udpproxy || true
    pkill -9 -f pytest || true

    if [ -f "keys.tdb.backup" ]; then
        echo "Restoring original keys.tdb from backup"
        mv keys.tdb.backup keys.tdb
    else
        echo "Removing test database"
        rm -f keys.tdb
    fi
}

# Set trap to ensure cleanup happens on normal exit too
trap cleanup EXIT

# Activate virtual environment
echo "Activating virtual environment..."
test -f venv/bin/activate && source venv/bin/activate

# Build UDPProxy
echo "Building UDPProxy..."
make clean
make distclean
make all

# Verify build
if [ ! -f "./udpproxy" ]; then
    echo "ERROR: UDPProxy build failed - executable not found"
    exit 1
fi

echo "✓ UDPProxy binary built successfully"

# Backup existing database if it exists
echo "Setting up test database..."
if [ -f "keys.tdb" ]; then
    echo "Backing up existing keys.tdb to keys.tdb.backup"
    mv keys.tdb keys.tdb.backup
fi

# Create fresh test database
echo "Initializing test database..."
python3 keydb.py initialise

# Run tests
echo ""
echo "=== Running Tests ==="

echo ""
echo "=== Running Connection Tests ==="
pytest tests/test_connections.py -v -s

echo ""
echo "=== Running Authentication Tests ==="
pytest tests/test_authentication.py -v -s

echo ""
echo "=== Test Summary ==="
echo "All tests completed successfully!"
echo "UDPProxy is ready for use."
