// Opens and closes WebRTC data channel connections between two PeerConnections in this process, over
// loopback, until one fails with "DTLS recv: Handshake failed: fatal I/O error". Prints that failure.
// Same as repro.mjs, but on libdatachannel directly.
//
//   ./repro [concurrency=20]
#include <openssl/crypto.h>
#include <rtc/rtc.hpp>
#include <rtc/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using State = rtc::PeerConnection::State;
using GatheringState = rtc::PeerConnection::GatheringState;

const auto TIMEOUT = 10s;

std::mutex logMutex;
std::vector<std::string> logLines; // libdatachannel's error log

// What one connection waits for. Set from libdatachannel's threads.
struct Events {
	std::mutex mutex;
	std::condition_variable cv;
	bool offererGathered = false, answererGathered = false, failed = false;
	int opened = 0;

	template <class F> void set(F f) {
		std::lock_guard lock(mutex);
		f();
		cv.notify_all();
	}
	template <class P> bool wait(P pred) {
		std::unique_lock lock(mutex);
		return cv.wait_for(lock, TIMEOUT, pred);
	}
};

// One connection: an offerer and an answerer, each with the same negotiated data channel.
// The offerer offers a=setup:actpass and libdatachannel answers a=setup:active, so the offerer is the
// DTLS server: it receives the ClientHello. That is the side that fails.
std::optional<std::string> connectOnce() {
	rtc::Configuration answererConfig;
	// the answerer listens on one UDP port for everyone, like a typical server (ICE UDP mux)
	answererConfig.portRangeBegin = answererConfig.portRangeEnd = 10000;
	answererConfig.enableIceUdpMux = true;
	auto offerer = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
	auto answerer = std::make_shared<rtc::PeerConnection>(answererConfig);
	struct Closer {
		std::shared_ptr<rtc::PeerConnection> a, b;
		~Closer() { a->close(); b->close(); }
	} closer{offerer, answerer};

	auto ev = std::make_shared<Events>();
	offerer->onGatheringStateChange([ev](GatheringState s) {
		if (s == GatheringState::Complete) ev->set([&] { ev->offererGathered = true; });
	});
	answerer->onGatheringStateChange([ev](GatheringState s) {
		if (s == GatheringState::Complete) ev->set([&] { ev->answererGathered = true; });
	});
	offerer->onStateChange([ev](State s) {
		if (s == State::Failed) ev->set([&] { ev->failed = true; });
	});

	rtc::DataChannelInit init;
	init.negotiated = true;
	init.id = 0;
	auto a = offerer->createDataChannel("data", init);
	auto b = answerer->createDataChannel("data", init);
	for (auto dc : {a, b})
		dc->onOpen([ev] { ev->set([&] { ev->opened++; }); });

	offerer->setLocalDescription();
	if (!ev->wait([&] { return ev->offererGathered; })) return "offerer gathering timed out";
	answerer->setRemoteDescription(*offerer->localDescription());
	if (!ev->wait([&] { return ev->answererGathered; })) return "answerer gathering timed out";
	offerer->setRemoteDescription(*answerer->localDescription());

	bool settled = ev->wait([&] { return ev->opened == 2 || ev->failed; });
	if (settled && !ev->failed) return std::nullopt;
	std::ostringstream out;
	out << "offerer " << (settled ? "failed" : "timed out") << " (state=" << offerer->state()
	    << ", ice=" << offerer->iceState() << ")";
	return out.str();
}

std::atomic<int> done = 0, other = 0;
std::mutex failureMutex;
std::condition_variable failureCv;
std::optional<std::string> dtlsFailure;

bool loggedSince(size_t before, const std::string &text) {
	std::lock_guard lock(logMutex);
	for (size_t i = before; i < logLines.size(); i++)
		if (logLines[i].find(text) != std::string::npos) return true;
	return false;
}

// Run until a connection fails with the DTLS error. Other failures are counted, not the point here.
void worker() {
	for (;;) {
		size_t before;
		{
			std::lock_guard lock(logMutex);
			before = logLines.size();
		}
		std::optional<std::string> failure;
		try {
			failure = connectOnce();
		} catch (const std::exception &e) {
			failure = std::string("threw: ") + e.what();
		}
		done++;
		if (failure && loggedSince(before, "fatal I/O error")) {
			std::lock_guard lock(failureMutex);
			if (!dtlsFailure) dtlsFailure = failure;
			failureCv.notify_all();
			return;
		}
		if (failure) other++;
	}
}

int main(int argc, char **argv) {
	const int concurrency = argc > 1 ? std::atoi(argv[1]) : 20;

	rtc::InitLogger(rtc::LogLevel::Error, [](rtc::LogLevel level, std::string message) {
		std::ostringstream line;
		line << level << ": " << message;
		std::lock_guard lock(logMutex);
		logLines.push_back(line.str());
	});
	rtc::Preload();
	for (int sig : {SIGINT, SIGTERM}) // also as PID 1 in Docker, where these are ignored by default
		std::signal(sig, [](int) { std::_Exit(130); });
	std::cout << "libdatachannel " << RTC_VERSION << ", " << OpenSSL_version(OPENSSL_VERSION) << std::endl;

	const auto started = std::chrono::steady_clock::now();
	for (int i = 0; i < concurrency; i++)
		std::thread(worker).detach();

	std::unique_lock lock(failureMutex);
	while (!failureCv.wait_for(lock, 10s, [] { return dtlsFailure.has_value(); })) {
		std::cout << done << " connections";
		if (other) std::cout << " (" << other << " failed some other way)";
		std::cout << ", no DTLS failure yet" << std::endl;
	}

	auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::cout << "\nFAILED after " << done << " connections (" << std::lround(seconds)
	          << " s): " << *dtlsFailure << "\nlibdatachannel errors:\n";
	std::vector<std::string> dtlsLines;
	{
		std::lock_guard logLock(logMutex);
		for (auto &line : logLines)
			if (line.find("DTLS") != std::string::npos) dtlsLines.push_back(line);
	}
	for (size_t i = dtlsLines.size() > 4 ? dtlsLines.size() - 4 : 0; i < dtlsLines.size(); i++)
		std::cout << "  " << dtlsLines[i] << "\n";
	std::cout << std::flush;
	std::_Exit(1); // don't wait for the connections still in progress
}
