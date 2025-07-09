#!/bin/bash
# Entrypoint script for UDPProxy Docker container

# Change to data directory for database operations
cd /app/data

# If first argument is a keydb.py command, run it
if [ "$1" = "python3" ] && [ "$2" = "keydb.py" ]; then
    shift 2
    exec python3 /app/keydb.py "$@"
# If first argument is keydb.py directly, run it with python3
elif [ "$1" = "keydb.py" ]; then
    shift
    exec python3 /app/keydb.py "$@"
# If first argument is bash or sh, run it
elif [ "$1" = "bash" ] || [ "$1" = "sh" ]; then
    exec "$@"
# Otherwise run udpproxy
else
    exec /app/udpproxy "$@"
fi
