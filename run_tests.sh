#!/bin/bash

# Test runner script for UDPProxy
# This script sets up the environment and runs all tests

set -e  # Exit on any error

echo "=== UDPProxy Test Runner ==="

# Cleanup function to restore database on exit
cleanup() {
    echo ""
    echo "=== Cleanup ==="
    if [ -f "keys.tdb.backup" ]; then
        echo "Restoring original keys.tdb from backup"
        mv keys.tdb.backup keys.tdb
    else
        echo "Removing test database"
        rm -f keys.tdb
    fi
}

# Set trap to ensure cleanup happens even if script fails
trap cleanup EXIT

# Check if virtual environment exists
if [ ! -d "venv" ]; then
    echo "Creating Python virtual environment with system site packages..."
    python3 -m venv --system-site-packages venv
fi

# Activate virtual environment
echo "Activating virtual environment..."
source venv/bin/activate

# Install/upgrade dependencies
echo "Installing Python dependencies..."
pip install --upgrade pip
pip install pytest pymavlink

# Ensure submodules are initialized
echo "Updating git submodules..."
git submodule update --init --recursive

# Build UDPProxy
echo "Building UDPProxy..."
make clean
make all

# Verify build
if [ ! -f "./udpproxy" ]; then
    echo "ERROR: UDPProxy build failed - executable not found"
    exit 1
fi

# Backup existing database if it exists
echo "Setting up test database..."
if [ -f "keys.tdb" ]; then
    echo "Backing up existing keys.tdb to keys.tdb.backup"
    mv keys.tdb keys.tdb.backup
fi

# Create fresh test database
python keydb.py initialise

# Run tests
echo "Running tests..."

# Run different test suites
echo ""
echo "=== Running UDP Connection Tests ==="
pytest tests/test_connections.py -v -s

echo ""
echo "=== Running Authentication Tests ==="
pytest tests/test_authentication.py -v -s

echo ""
echo "=== Test Summary ==="
echo "All tests completed successfully!"
echo "UDPProxy is ready for use."
