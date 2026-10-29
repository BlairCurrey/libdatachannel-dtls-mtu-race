# Builds libdatachannel v0.24.6 from source and runs repro.cpp.
#   docker build -t dtls-repro . && docker run --rm dtls-repro
# Variants: --build-arg WIDEN_WINDOW=ON (widen-window.patch), --build-arg FIX=ON (fix.patch)
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake git libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY CMakeLists.txt repro.cpp fix.patch widen-window.patch ./
ARG WIDEN_WINDOW=OFF
ARG FIX=OFF
RUN cmake -B build -DWIDEN_WINDOW=$WIDEN_WINDOW -DFIX=$FIX \
    && cmake --build build -j"$(nproc)"
CMD ["build/repro"]
