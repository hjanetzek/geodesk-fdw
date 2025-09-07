# Multi-stage build for GeoDesk FDW
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    postgresql-14 \
    postgresql-server-dev-14 \
    libgeos-dev \
    libproj-dev \
    libjson-c-dev \
    libsfcgal-dev \
    libxml2-dev \
    libgdal-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
WORKDIR /build
COPY . .

# Fetch and build dependencies
RUN ./fetch-dependencies.sh

# Build the extension
RUN make clean && make

# Final image with just PostgreSQL and the extension
FROM postgres:14-bullseye

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    postgresql-14-postgis-3 \
    libgeos-c1v5 \
    libproj19 \
    libjson-c5 \
    libsfcgal1 \
    && rm -rf /var/lib/apt/lists/*

# Copy the built extension from builder
COPY --from=builder /build/*.so /usr/lib/postgresql/14/lib/
COPY --from=builder /build/*.control /usr/share/postgresql/14/extension/
COPY --from=builder /build/sql/*.sql /usr/share/postgresql/14/extension/

# Initialize PostGIS and GeoDesk FDW
RUN echo "CREATE EXTENSION IF NOT EXISTS postgis;" > /docker-entrypoint-initdb.d/01-postgis.sql
RUN echo "CREATE EXTENSION IF NOT EXISTS geodesk_fdw;" > /docker-entrypoint-initdb.d/02-geodesk-fdw.sql

# Set default environment
ENV POSTGRES_PASSWORD=postgres
ENV POSTGRES_DB=osm

EXPOSE 5432

# Add a healthcheck
HEALTHCHECK --interval=10s --timeout=5s --retries=5 \
  CMD pg_isready -U postgres || exit 1