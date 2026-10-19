# ---------- stage 1: build + run unit tests ----------
FROM ubuntu:24.04 AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make libboost-dev nlohmann-json3-dev libgtest-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt .
COPY src src
COPY tests tests
COPY bench bench
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" \
 && ctest --test-dir build --output-on-failure

# ---------- stage 2: small runtime image ----------
# Only the binary + web UI are copied: no compiler, no headers, no sources.
FROM ubuntu:24.04
RUN useradd --system --uid 10001 nx && mkdir -p /data && chown nx /data
WORKDIR /app
COPY --from=build /src/build/nanoexchange /app/nanoexchange
COPY web /app/web
ENV PORT=8080 DATA_DIR=/data STATIC_DIR=/app/web THREADS=2 BOTS=1
USER nx
EXPOSE 8080
VOLUME ["/data"]
HEALTHCHECK --interval=10s --timeout=3s --retries=3 \
  CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/${PORT} && printf "GET /health HTTP/1.0\r\n\r\n" >&3 && grep -q "\"ok\"" <&3'
ENTRYPOINT ["/app/nanoexchange"]
