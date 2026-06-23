# Multi-stage build for PayBackend
# Stage 1: Build stage
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during build
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    cmake \
    build-essential \
    libpq-dev \
    libssl-dev \
    wget \
    git \
    unzip \
    python3 \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Install Conan
RUN pip3 install conan

# Set working directory
WORKDIR /build

# Copy source code
COPY . .

# Build application
RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) && \
    make install

# Stage 2: Runtime stage
FROM ubuntu:22.04

# Install runtime dependencies only
RUN apt-get update && apt-get install -y \
    libpq5 \
    openssl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -m -u 1000 payuser && \
    mkdir -p /app/config /app/certs /app/logs && \
    chown -R payuser:payuser /app

# Set working directory
WORKDIR /app

# Copy built binaries from builder stage
COPY --from=builder /build/build/Release/PayServer /app/PayServer
COPY --from=builder /build/build/Release/PayBackendTest /app/PayBackendTest

# Copy configuration template
COPY config.example.json /app/config.example.json

# Change ownership
RUN chown payuser:payuser /app/PayServer /app/PayBackendTest /app/config.example.json

# Switch to non-root user
USER payuser

# Expose port
EXPOSE 5566

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:5566/healthz || exit 1

# Run application
CMD ["./PayServer"]
