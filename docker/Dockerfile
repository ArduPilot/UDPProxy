# Dockerfile for UDPProxy - Multi-stage build (Ubuntu version)
# Build stage
FROM ubuntu:24.04 AS builder

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install --no-install-recommends -y \
    g++ \
    make \
    git \
    python3 \
    python3-pip \
    python3-venv \
    libtdb-dev \
    libssl-dev \
    libcrypto++-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/*

# Set working directory
WORKDIR /app

# Copy source code (from parent directory)
COPY . .

# Create virtual environment and install pymavlink
RUN python3 -m venv venv && \
    . venv/bin/activate && \
    pip install --no-cache-dir pymavlink

# Initialize git submodules and build
RUN git submodule update --init --recursive || true

# Build the udpproxy binary with venv activated
RUN . venv/bin/activate && make clean && make

# Runtime stage
FROM ubuntu:24.04 AS runtime

# Avoid interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install only runtime dependencies
RUN apt-get update && apt-get install --no-install-recommends -y \
    python3 \
    python3-tdb \
    libtdb1 \
    libssl3 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /app

# Copy the built binary and necessary files from builder stage
COPY --from=builder /app/udpproxy /app/udpproxy
COPY --from=builder /app/keydb.py /app/keydb.py

# Create data directory for persistent storage
RUN mkdir -p /app/data

# Copy entrypoint script
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# Use the entrypoint script
ENTRYPOINT ["/app/entrypoint.sh"]

# Default command is to run udpproxy
CMD []
