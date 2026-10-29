# libdatachannel: rare `DTLS recv: Handshake failed: fatal I/O error`

Roughly 1 connection in several thousand to tens of thousands fails right after ICE connects. The side that receives the ClientHello logs:

```
DtlsTransport::doRecv@1021: DTLS recv: Handshake failed: fatal I/O error
DtlsTransport::doRecv@1029: DTLS handshake failed
```

**Cause:** `DtlsTransport::start()` (OpenSSL backend) registers for incoming packets *before* it sets the MTU. If the ClientHello arrives in between, OpenSSL handles it with an MTU of 0 and fails silently.

**Affected:** v0.24.6 and master, OpenSSL 3.5 and 3.6, Linux and macOS.

## Reproduce

Clone this repo, then build and run in Docker (the first build compiles libdatachannel, so it takes a few minutes):

```sh
git clone https://github.com/BlairCurrey/libdatachannel-dtls-mtu-race
cd libdatachannel-dtls-mtu-race
docker build -t dtls-repro . && docker run --rm dtls-repro
```

[`repro.cpp`](repro.cpp) uses libdatachannel v0.24.6 directly, with no bindings. It connects pairs of `PeerConnection`s over loopback, 20 at a time, until one fails with this error. It usually fails within a few minutes:

```
FAILED after 14611 connections (55 s): offerer failed (state=closed, ice=closed)
libdatachannel errors:
  error: rtc::impl::DtlsTransport::doRecv@1021: DTLS recv: Handshake failed: fatal I/O error
  error: rtc::impl::DtlsTransport::doRecv@1029: DTLS handshake failed
```

To make it fail immediately, or to try the fix, add build args:

```sh
# fails within the first few connections
docker build -t dtls-widened --build-arg WIDEN_WINDOW=ON . && docker run --rm dtls-widened
# no failures (runs until Ctrl-C)
docker build -t dtls-fixed --build-arg WIDEN_WINDOW=ON --build-arg FIX=ON . && docker run --rm dtls-fixed
```

Without Docker (CMake 3.24+, C++17, OpenSSL): `cmake -B build && cmake --build build && build/repro`, with `-DWIDEN_WINDOW=ON` / `-DFIX=ON` for the patches.

## What happens

1. The offerer offers `actpass` and libdatachannel answers `active`, so the offerer is the DTLS server.
2. [`start()`](https://github.com/paullouisageneau/libdatachannel/blob/v0.24.6/src/impl/dtlstransport.cpp#L857-L871) calls `registerIncoming()` first and `SSL_set_mtu()` a few lines later.
3. The answerer's ClientHello lands in that window (live, or buffered and replayed by `registerIncoming()` since [#1554](https://github.com/paullouisageneau/libdatachannel/pull/1554)), and `doRecv()` takes `mSslMutex` before `start()` does.
4. `doRecv()` runs `SSL_do_handshake()` with MTU 0. With `SSL_OP_NO_QUERY_MTU` set, OpenSSL's [`dtls1_do_write()`](https://github.com/openssl/openssl/blob/openssl-3.6.0/ssl/statem/statem_dtls.c#L118-L119) returns -1 without queuing an error, so `SSL_get_error()` gives `SSL_ERROR_SYSCALL`: "fatal I/O error".

## Fix

[`fix.patch`](fix.patch) (against v0.24.6; applies to master with `patch -p1`) moves `registerIncoming()` and `changeState(Connecting)` below the block that sets the MTU and runs the first `SSL_do_handshake()`. An early ClientHello is still buffered by #1554 and handled right after, now with the MTU set.

With the patch applied to master, libdatachannel's own CI passes (all six workflows, Linux, macOS and Windows): [run on a fork](https://github.com/BlairCurrey/libdatachannel/commit/c299928).

Moving only `SSL_set_mtu()` earlier also stops the failures, but leaves `doRecv()` holding `mSslMutex` while `start()` waits for it on the libjuice thread, the lock-order hazard the comment in `start()` warns about.

| Docker, Linux | result |
| --- | --- |
| unpatched | failed in 5 of 5 runs, after 6,100 to 57,700 connections |
| unpatched, window widened ([`widen-window.patch`](widen-window.patch), a 2 ms sleep) | failed in 8 of 8 runs, within the first few connections |
| `fix.patch` | 0 failures in 78,926 connections |
| `fix.patch`, window widened | 0 failures in 16,101 connections |

## Not #1632

[#1632](https://github.com/paullouisageneau/libdatachannel/issues/1632) has the same message but was a different race (unlocked `BIO_write`, fixed in v0.24.4). Its error comes from `start()`; this one comes from `doRecv()`.
