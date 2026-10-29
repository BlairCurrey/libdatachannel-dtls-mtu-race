// Opens and closes WebRTC data channel connections between two PeerConnections in this process, over
// loopback, until one fails with "DTLS recv: Handshake failed: fatal I/O error". Prints that failure.
//
//   node repro.mjs [concurrency=20]
import ndc from 'node-datachannel';

const CONCURRENCY = Number(process.argv[2] ?? 20);
const TIMEOUT_MS = 10_000;

const log = [];
ndc.initLogger('Error', (level, message) => log.push(`${level}: ${message}`));

// One connection: an offerer and an answerer, each with the same negotiated data channel.
// The offerer offers a=setup:actpass and libdatachannel answers a=setup:active, so the offerer is the
// DTLS server: it receives the ClientHello. That is the side that fails.
async function connectOnce() {
  const offerer = new ndc.PeerConnection('offerer', { iceServers: [] });
  // the answerer listens on one UDP port for everyone, like a typical server (ICE UDP mux)
  const answerer = new ndc.PeerConnection('answerer', { iceServers: [], portRangeBegin: 10000, portRangeEnd: 10000, enableIceUdpMux: true });
  const gathered = pc => new Promise(resolve => {
    if (pc.gatheringState() === 'complete') return resolve();
    pc.onGatheringStateChange(state => state === 'complete' && resolve());
  });
  try {
    const a = offerer.createDataChannel('data', { negotiated: true, id: 0 });
    const b = answerer.createDataChannel('data', { negotiated: true, id: 0 });
    const opened = Promise.all([a, b].map(dc => new Promise(resolve => dc.onOpen(resolve))));
    const failed = new Promise(resolve => offerer.onStateChange(state => state === 'failed' && resolve('failed')));

    offerer.setLocalDescription();
    await gathered(offerer);
    answerer.setRemoteDescription(offerer.localDescription().sdp, 'offer');
    await gathered(answerer);
    offerer.setRemoteDescription(answerer.localDescription().sdp, 'answer');

    const timeout = new Promise(resolve => setTimeout(resolve, TIMEOUT_MS, 'timed out'));
    const result = await Promise.race([opened.then(() => 'ok'), failed, timeout]);
    return result === 'ok' ? null : `offerer ${result} (state=${offerer.state()}, ice=${offerer.iceState()})`;
  } finally {
    offerer.close();
    answerer.close();
  }
}

let done = 0, other = 0;
const started = Date.now();
const progress = setInterval(() => console.log(`${done} connections${other ? ` (${other} failed some other way)` : ''}, no DTLS failure yet`), 10_000);

// Run until a connection fails with the DTLS error. Other failures are counted, not the point here.
async function worker() {
  for (;;) {
    const before = log.length;
    const failure = await connectOnce().catch(e => `threw: ${e.message}`);
    done++;
    if (failure && log.slice(before).some(l => l.includes('fatal I/O error'))) return failure;
    if (failure) other++;
  }
}

const failure = await Promise.race(Array.from({ length: CONCURRENCY }, worker));
clearInterval(progress);
console.log(`\nFAILED after ${done} connections (${((Date.now() - started) / 1000).toFixed(0)} s): ${failure}`);
console.log('libdatachannel errors:\n  ' + log.filter(l => /DTLS/.test(l)).slice(-4).join('\n  '));
process.exit(1);
