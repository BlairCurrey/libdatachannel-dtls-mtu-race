# libdatachannel: rare `DTLS recv: Handshake failed: fatal I/O error`

Roughly 1 connection in 2,000 to 20,000 fails right after ICE connects. The side that receives the ClientHello logs this:

```
DtlsTransport::doRecv@1021: DTLS recv: Handshake failed: fatal I/O error
DtlsTransport::doRecv@1029: DTLS handshake failed
```

**Cause:** `DtlsTransport::start()` (the OpenSSL backend) registers for incoming packets *before* it sets the MTU. If the ClientHello arrives in between, OpenSSL tries to send its reply with an MTU of 0 and fails silently.

**Affected:** libdatachannel v0.24.6 (latest release) and master ([`bdc5ff2`](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/impl/dtlstransport.cpp#L850-L864)), with OpenSSL 3.6.3. Seen on Linux arm64 (Docker) and macOS arm64.

## Reproduce

```sh
docker build -t dtls-repro . && docker run --rm dtls-repro
```

or, without Docker: `npm install && node repro.mjs`

[`repro.mjs`](repro.mjs) is about 60 lines. It uses [node-datachannel](https://github.com/murat-dogan/node-datachannel) 0.33.6, which bundles libdatachannel 0.24.6. It connects pairs of `PeerConnection`s over loopback in one process, 20 at a time, and stops at the first failure with this error. Other failures, such as a timeout on a busy machine, are counted and skipped.

In four runs on Linux in Docker, it failed after 26 s, 70 s, 6.5 min and 11 min (2,400 to 23,000 connections):

```
2261 connections (20 failed some other way), no DTLS failure yet

FAILED after 2368 connections (26 s): offerer failed (state=closed, ice=closed)
libdatachannel errors:
  Error: rtc::impl::DtlsTransport::doRecv@1021: DTLS recv: Handshake failed: fatal I/O error
  Error: rtc::impl::DtlsTransport::doRecv@1029: DTLS handshake failed
```

The answerer uses ICE UDP mux on one port, like a server would. Without the mux, the failure is much rarer: none in 12,000 connections.

## What happens

1. The offerer offers `actpass`, and libdatachannel answers `active`, so the offerer is the DTLS server.
2. [`start()`](https://github.com/paullouisageneau/libdatachannel/blob/v0.24.6/src/impl/dtlstransport.cpp#L857-L871) calls `registerIncoming()` first and `SSL_set_mtu()` a few lines later.
3. The answerer's ClientHello reaches the offerer in that window. It can arrive live, or arrive early and be replayed by `registerIncoming()`, which [#1554](https://github.com/paullouisageneau/libdatachannel/pull/1554) added. Either way it goes to the thread pool, and `doRecv()` can take `mSslMutex` before `start()` does.
4. So `doRecv()` runs `SSL_do_handshake()` while the MTU is still 0. Because `SSL_OP_NO_QUERY_MTU` is set, [`dtls1_query_mtu()`](https://github.com/openssl/openssl/blob/openssl-3.6.0/ssl/d1_lib.c#L915) returns 0. Then [`dtls1_do_write()`](https://github.com/openssl/openssl/blob/openssl-3.6.0/ssl/statem/statem_dtls.c#L118-L119) returns -1 without queuing an error, `SSL_get_error()` falls through to `SSL_ERROR_SYSCALL`, and that becomes "fatal I/O error".

When I instrumented a failure, it showed one 215-byte ClientHello in, `ret=-1 err=5`, OpenSSL state `TWSH` (writing the ServerHello), and an empty error queue.

## Fix

[`fix.patch`](fix.patch) moves `registerIncoming()` and `changeState(Connecting)` below the block that sets the MTU and runs the first `SSL_do_handshake()`. A ClientHello that arrives early is still buffered by #1554, so it gets handled right after, now with the MTU set.

Moving only `SSL_set_mtu()` above `registerIncoming()` also stops the failures. But then `doRecv()` can hold `mSslMutex` while it sends the server flight, while `start()` waits for that lock on the libjuice thread. That looks like the lock-order hazard the comment in `start()` warns about.

Results, with the race window widened by a 2 ms sleep ([`widen-window.patch`](widen-window.patch), which needs a source build):

| | failures |
| --- | --- |
| unpatched, window widened | 1,152 in under 40 s |
| `fix.patch`, window widened | 0 in 2,025 connections |

Without the widening, a 40-minute load test went from about 1 failure per 10,000 joins to 0 in 38,458.

## Not #1632

[#1632](https://github.com/paullouisageneau/libdatachannel/issues/1632) had the same error message. It was a different race: an unlocked `BIO_write`, fixed in v0.24.4. There, the error comes from `DtlsTransport::start`; here it comes from `DtlsTransport::doRecv`.
