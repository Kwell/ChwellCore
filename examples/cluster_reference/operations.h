#pragma once
#include "chwell/metrics/prometheus_metrics.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace reference {
using Clock = std::chrono::steady_clock;

// Only cached state crosses the operations boundary; probes never perform DB/RPC IO.
class OperationsState {
    chwell::metrics::PrometheusRegistry registry_;
public:
    OperationsState();
    void main_progress();
    void worker_progress();
    void started();
    void dependencies(bool database, bool discovery, std::size_t nodes);
    void drain();
    void finished();
    void stopped();
    bool ready() const;
    bool live() const;
    bool complete() const;
    std::string health(bool readiness) const;
    std::string metrics();
    chwell::metrics::Counter &requests, &errors, &rejected, &dropped;
    chwell::metrics::Gauge &queued, &inflight, &sessions;
private:
    bool ready_locked(Clock::time_point now) const;
    bool live_locked(Clock::time_point now) const;
    mutable std::mutex mutex_;
    Clock::time_point main_ = Clock::now(), worker_ = Clock::now(), checked_{};
    bool started_ = false, draining_ = false, finished_ = false, stopped_ = false;
    bool database_ = false, discovery_ = false;
    std::size_t nodes_ = 0;
};

// Small GET-only operations listener, with bounded clients, headers and deadlines.
// Linux/POSIX example infrastructure, deliberately separate from the business inbox.
class OperationsServer {
public:
    OperationsServer(OperationsState& state, int port, const std::string& address);
    ~OperationsServer();
    OperationsServer(const OperationsServer&) = delete;
    OperationsServer& operator=(const OperationsServer&) = delete;
private:
    void run();
    OperationsState& state_;
    int listener_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
int probe(int port, const std::string& kind);
}
