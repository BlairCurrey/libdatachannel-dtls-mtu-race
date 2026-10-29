# libdatachannel: rare `DTLS recv: Handshake failed: fatal I/O error`

Roughly 1 connection in a few thousand to a few tens of thousands fails right after ICE connects. The side that receives the ClientHello logs this:

```
DtlsTransport::doRecv@1021: DTLS recv: Handshake failed: fatal I/O error
DtlsTransport::doRecv@1029: DTLS handshake failed
```

**Cause:** `DtlsTransport::start()` (the OpenSSL backend) registers for incoming packets *before* it sets the MTU. If the ClientHello arrives in between, OpenSSL tries to send its reply with an MTU of 0 and fails silently.

**Affected:** libdatachannel v0.24.6 (latest release) and master ([`bdc5ff2`](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/impl/dtlstransport.cpp#L850-L864)), with OpenSSL 3.5.7 (Debian trixie, used by the Docker build here) and 3.6.3 (Homebrew on macOS). Also seen with OpenSSL 3.6.3 through node-datachannel. Seen on Linux arm64 (Docker) and macOS arm64.

## Reproduce

```sh
docker build -t dtls-repro . && docker run --rm dtls-repro
```

This uses libdatachannel directly, with no bindings. I first found the bug through node-datachannel.

[`repro.cpp`](repro.cpp) is about 170 lines. [`CMakeLists.txt`](CMakeLists.txt) fetches libdatachannel v0.24.6 and builds it with OpenSSL, without media or WebSocket support. The repro connects pairs of `PeerConnection`s over loopback in one process, 20 at a time, and stops at the first failure with this error. Other failures, such as a timeout on a busy machine, are counted and skipped.

In five runs on Linux in Docker, it failed after 23 s, 55 s, 2.8 min, 3.2 min and 3.6 min (6,100 to 57,700 connections):

```
libdatachannel 0.24.6, OpenSSL 3.5.7 9 Jun 2026
...
10516 connections, no DTLS failure yet
13034 connections, no DTLS failure yet

FAILED after 14611 connections (55 s): offerer failed (state=closed, ice=closed)
libdatachannel errors:
  error: rtc::impl::DtlsTransport::doRecv@1021: DTLS recv: Handshake failed: fatal I/O error
  error: rtc::impl::DtlsTransport::doRecv@1029: DTLS handshake failed
```

The answerer uses ICE UDP mux on one port, like a server would. With node-datachannel, the mux mattered: without it, none failed in 12,000 connections. In this C++ repro it makes no clear difference: without the mux, 4 of 4 runs still failed, after 2,500 to 51,500 connections.

Without Docker, you need CMake 3.24+, a C++17 compiler and OpenSSL (on macOS, add `-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)`):

```sh
cmake -B build && cmake --build build && build/repro
```

Built this way on macOS arm64 with OpenSSL 3.6.3, it failed after 1 s and 6.3 min (288 and 23,500 connections).

To apply the patches below, pass `--build-arg WIDEN_WINDOW=ON` and/or `--build-arg FIX=ON` to `docker build` (or `-DWIDEN_WINDOW=ON` / `-DFIX=ON` to `cmake`):

```sh
docker build -t dtls-repro-widened --build-arg WIDEN_WINDOW=ON . && docker run --rm dtls-repro-widened
docker build -t dtls-repro-fixed --build-arg WIDEN_WINDOW=ON --build-arg FIX=ON . && docker run --rm dtls-repro-fixed
```

The fixed build runs until you stop it with Ctrl-C.

[`repro.mjs`](repro.mjs) is the original test, through [node-datachannel](https://github.com/murat-dogan/node-datachannel) 0.33.6 (which bundles libdatachannel 0.24.6), if you'd rather not compile: `npm install && node repro.mjs`. In four Docker runs it failed after 26 s to 11 min.

## What happens

1. The offerer offers `actpass`, and libdatachannel answers `active`, so the offerer is the DTLS server.
2. [`start()`](https://github.com/paullouisageneau/libdatachannel/blob/v0.24.6/src/impl/dtlstransport.cpp#L857-L871) calls `registerIncoming()` first and `SSL_set_mtu()` a few lines later.
3. The answerer's ClientHello reaches the offerer in that window. It can arrive live, or arrive early and be replayed by `registerIncoming()`, which [#1554](https://github.com/paullouisageneau/libdatachannel/pull/1554) added. Either way it goes to the thread pool, and `doRecv()` can take `mSslMutex` before `start()` does.
4. So `doRecv()` runs `SSL_do_handshake()` while the MTU is still 0. Because `SSL_OP_NO_QUERY_MTU` is set, [`dtls1_query_mtu()`](https://github.com/openssl/openssl/blob/openssl-3.6.0/ssl/d1_lib.c#L915) returns 0. Then [`dtls1_do_write()`](https://github.com/openssl/openssl/blob/openssl-3.6.0/ssl/statem/statem_dtls.c#L118-L119) returns -1 without queuing an error, `SSL_get_error()` falls through to `SSL_ERROR_SYSCALL`, and that becomes "fatal I/O error".

When I instrumented a failure, it showed one 215-byte ClientHello in, `ret=-1 err=5`, OpenSSL state `TWSH` (writing the ServerHello), and an empty error queue.

## Fix

[`fix.patch`](fix.patch) moves `registerIncoming()` and `changeState(Connecting)` below the block that sets the MTU and runs the first `SSL_do_handshake()`. A ClientHello that arrives early is still buffered by #1554, so it gets handled right after, now with the MTU set.

Moving only `SSL_set_mtu()` above `registerIncoming()` also stops the failures. But then `doRecv()` can hold `mSslMutex` while it sends the server flight, while `start()` waits for that lock on the libjuice thread. That looks like the lock-order hazard the comment in `start()` warns about.

Results in Docker. [`widen-window.patch`](widen-window.patch) widens the race window with a 2 ms sleep just before the MTU is set. With `fix.patch` applied too, that sleep comes before `registerIncoming()`, so the ClientHello is buffered and replayed after it.

| | result |
| --- | --- |
| unpatched | fails after 6,100 to 57,700 connections (5 of 5 runs) |
| unpatched, window widened | fails in under 1 s, within the first few connections (8 of 8 runs) |
| `fix.patch` | 0 failures in 78,926 connections (5 min) |
| `fix.patch`, window widened | 0 failures in 16,101 connections (100 s) |

Without the widening, a 40-minute load test went from about 1 failure per 10,000 joins to 0 in 38,458.

## Not #1632

[#1632](https://github.com/paullouisageneau/libdatachannel/issues/1632) had the same error message. It was a different race: an unlocked `BIO_write`, fixed in v0.24.4. There, the error comes from `DtlsTransport::start`; here it comes from `DtlsTransport::doRecv`.
