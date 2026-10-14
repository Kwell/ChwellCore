#include "chwell/net/epoll_demuxer.h"
#include "chwell/net/epoll_connection.h"
#include "chwell/net/epoll_server.h"
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

using chwell::net::EpollDemuxer;
using chwell::net::IoDemuxer;
using chwell::net::IoEvent;
using chwell::net::EventCallback;
using chwell::net::has_event;
using namespace std::chrono_literals;

namespace {
struct Pair {
    int fd[2]{-1, -1};
    Pair() {
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fd) != 0)
            throw std::runtime_error("socketpair");
    }
    ~Pair() { for (int value : fd) if (value >= 0) ::close(value); }
    int take(int index) { return std::exchange(fd[index], -1); }
};

// Always stop/join before returning, including failed assertions and exceptions.
// Tests whose callbacks never arrive fail rather than hang in epoll_wait.
bool run_to_stop(IoDemuxer& demuxer) {
    std::promise<void> finished;
    auto done = finished.get_future();
    std::exception_ptr error;
    std::thread runner([&] {
        try { demuxer.run(); } catch (...) { error = std::current_exception(); }
        finished.set_value();
    });
    const bool ready = done.wait_for(3s) == std::future_status::ready;
    demuxer.stop();
    runner.join();
    if (error) std::rethrow_exception(error);
    return ready;
}
}

TEST(EpollDemuxerBoundary, InvalidCapacityAndDescriptorsFailWithoutRegistration) {
    for (int size : {0, -1}) {
        EpollDemuxer invalid(size);
        EXPECT_FALSE(invalid.is_valid());
        EXPECT_EQ(invalid.native_handle(), -1);
        EXPECT_FALSE(invalid.wake());
        EXPECT_EQ(errno, EBADF);
        invalid.run();
    }
    EpollDemuxer demuxer;
    ASSERT_TRUE(demuxer.is_valid());
    EXPECT_FALSE(demuxer.add(-1, IoEvent::Read, {}));
    EXPECT_EQ(errno, EBADF);
    EXPECT_FALSE(demuxer.set_callback(-1, {}));
    EXPECT_FALSE(demuxer.modify(-1, IoEvent::Read));
    EXPECT_FALSE(demuxer.remove(-1));
    EXPECT_FALSE(chwell::net::set_nonblocking(-1));
    const int fd = chwell::net::create_nonblocking_socket(AF_INET, SOCK_STREAM);
    ASSERT_GE(fd, 0);
    EXPECT_NE(::fcntl(fd, F_GETFL) & O_NONBLOCK, 0);
    ::close(fd);
}

TEST(EpollDemuxerBoundary, DuplicateAddPreservesOriginalCallbackAndInterest) {
    Pair pair;
    EpollDemuxer demuxer;
    int original = 0, replacement = 0;
    ASSERT_TRUE(demuxer.add(pair.fd[0], IoEvent::Read, [&](int fd, IoEvent events) {
        EXPECT_EQ(fd, pair.fd[0]);
        EXPECT_TRUE(has_event(events, IoEvent::Read));
        EXPECT_FALSE(has_event(events, IoEvent::Write));
        ++original;
        demuxer.stop();
    }));
    EXPECT_FALSE(demuxer.add(pair.fd[0], IoEvent::Write, [&](int, IoEvent) {
        ++replacement; demuxer.stop();
    }));
    EXPECT_EQ(errno, EEXIST);
    ASSERT_EQ(::write(pair.fd[1], "x", 1), 1);
    EXPECT_TRUE(run_to_stop(demuxer));
    EXPECT_EQ(original, 1);
    EXPECT_EQ(replacement, 0);
}

TEST(EpollDemuxerBoundary, FailedNativeAddReleasesCapturesAndLeavesNoCallback) {
    EpollDemuxer demuxer;
    auto capture = std::make_shared<int>(42);
    std::weak_ptr<int> weak = capture;
    // Regular files cannot be registered with epoll (EPERM).
    const int file = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(file, 0);
    EXPECT_FALSE(demuxer.add(file, IoEvent::Read, [capture](int, IoEvent) {}));
    EXPECT_EQ(errno, EPERM);
    capture.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_FALSE(demuxer.set_callback(file, {}));
    EXPECT_EQ(errno, ENOENT);
    ::close(file);
}

TEST(EpollDemuxerBoundary, InterestTranslationAndCallbackReplacementWorkThroughInterface) {
    Pair pair;
    EpollDemuxer backend;
    IoDemuxer& demuxer = backend;
    int old_calls = 0, new_calls = 0;
    ASSERT_TRUE(demuxer.add(pair.fd[0], IoEvent::Read, [&](int, IoEvent) { ++old_calls; }));
    ASSERT_TRUE(demuxer.modify(pair.fd[0], IoEvent::Write));
    ASSERT_TRUE(demuxer.set_callback(pair.fd[0], [&](int, IoEvent events) {
        ++new_calls;
        EXPECT_TRUE(has_event(events, IoEvent::Write));
        EXPECT_FALSE(has_event(events, IoEvent::Read));
        demuxer.stop();
    }));
    EXPECT_TRUE(run_to_stop(demuxer));
    EXPECT_EQ(old_calls, 0);
    EXPECT_EQ(new_calls, 1);
}

TEST(EpollDemuxerBoundary, InvalidModifyDoesNotChangeReadInterest) {
    Pair pair;
    EpollDemuxer demuxer;
    ASSERT_TRUE(demuxer.add(pair.fd[0], IoEvent::Read, [&](int, IoEvent events) {
        EXPECT_TRUE(has_event(events, IoEvent::Read));
        EXPECT_FALSE(has_event(events, IoEvent::Write));
        demuxer.stop();
    }));
    EXPECT_FALSE(demuxer.modify(pair.fd[0], static_cast<IoEvent>(0x80000000u)));
    EXPECT_EQ(errno, EINVAL);
    ASSERT_EQ(::write(pair.fd[1], "x", 1), 1);
    EXPECT_TRUE(run_to_stop(demuxer));
}

TEST(EpollDemuxerBoundary, ClosedRegistrationCanBeRetiredAndDescriptorReused) {
    Pair pair, replacement;
    EpollDemuxer demuxer;
    const int fd = pair.take(0);
    ASSERT_TRUE(demuxer.add(fd, IoEvent::Read, {}));
    ::close(fd);
    EXPECT_FALSE(demuxer.modify(fd, IoEvent::Write));
    EXPECT_EQ(errno, EBADF);
    EXPECT_FALSE(demuxer.remove(fd));
    EXPECT_EQ(errno, EBADF);
    ASSERT_EQ(::dup2(replacement.fd[0], fd), fd);
    pair.fd[0] = fd;
    ASSERT_TRUE(demuxer.add(fd, IoEvent::Write, [&](int, IoEvent events) {
        EXPECT_TRUE(has_event(events, IoEvent::Write)); demuxer.stop();
    }));
    EXPECT_TRUE(run_to_stop(demuxer));
}

TEST(EpollDemuxerBoundary, BufferedEventsCannotReachReregisteredOrReusedDescriptor) {
    for (bool reuse : {false, true}) {
        SCOPED_TRACE(reuse);
        Pair pairs[2], replacement, sentinel;
        EpollDemuxer demuxer(8);
        int originals = 0, stale = 0, sentinels = 0;
        for (int index = 0; index < 2; ++index) {
            ASSERT_TRUE(demuxer.add(pairs[index].fd[0], IoEvent::Read, [&, index](int fd, IoEvent) {
                ++originals;
                EXPECT_TRUE(demuxer.remove(fd));
                const int other = pairs[1 - index].fd[0];
                EXPECT_TRUE(demuxer.remove(other));
                if (reuse) {
                    ::close(other);
                    EXPECT_EQ(::dup2(replacement.fd[0], other), other);
                } else {
                    char value;
                    EXPECT_EQ(::read(other, &value, 1), 1);
                }
                // No native readiness is requested for the new registration.
                EXPECT_TRUE(demuxer.add(other, IoEvent::None, [&](int, IoEvent) { ++stale; }));
                // This readiness enters the NEXT wait, after the old batch was inspected.
                EXPECT_TRUE(demuxer.add(sentinel.fd[0], IoEvent::Read, [&](int, IoEvent) {
                    ++sentinels; demuxer.stop();
                }));
                EXPECT_EQ(::write(sentinel.fd[1], "s", 1), 1);
            }));
            ASSERT_EQ(::write(pairs[index].fd[1], "x", 1), 1);
        }
        EXPECT_TRUE(run_to_stop(demuxer));
        EXPECT_EQ(originals, 1);
        EXPECT_EQ(stale, 0);
        EXPECT_EQ(sentinels, 1);
    }
}

TEST(EpollDemuxerBoundary, HalfCloseAndHangupTranslateNativeFlags) {
    for (bool half_close : {false, true}) {
        Pair pair;
        EpollDemuxer demuxer;
        ASSERT_TRUE(demuxer.add(pair.fd[0], IoEvent::Read | IoEvent::RdHangup, [&](int, IoEvent events) {
            EXPECT_TRUE(has_event(events, IoEvent::Read));
            EXPECT_TRUE(has_event(events, IoEvent::RdHangup));
            if (!half_close) EXPECT_TRUE(has_event(events, IoEvent::Hangup));
            demuxer.stop();
        }));
        if (half_close) ASSERT_EQ(::shutdown(pair.fd[1], SHUT_WR), 0);
        else ::close(pair.take(1));
        EXPECT_TRUE(run_to_stop(demuxer));
    }
}

TEST(EpollDemuxerBoundary, NativeErrorIsReportedEvenWithoutExplicitErrorInterest) {
    int pipe_fds[2];
    ASSERT_EQ(::pipe2(pipe_fds, O_NONBLOCK | O_CLOEXEC), 0);
    EpollDemuxer demuxer;
    ASSERT_TRUE(demuxer.add(pipe_fds[1], IoEvent::None, [&](int, IoEvent events) {
        EXPECT_TRUE(has_event(events, IoEvent::Error));
        demuxer.stop();
    }));
    ::close(pipe_fds[0]);
    EXPECT_TRUE(run_to_stop(demuxer));
    ::close(pipe_fds[1]);
}

TEST(EpollDemuxerBoundary, EdgeTriggerArgumentAndMaskDoNotRepeatedlyDispatchUnreadData) {
    for (bool argument : {false, true}) {
        Pair pair, sentinel;
        EpollDemuxer demuxer;
        int calls = 0;
        const auto events = argument ? IoEvent::Read : IoEvent::Read | IoEvent::EdgeTrigger;
        ASSERT_TRUE(demuxer.add(pair.fd[0], events, [&](int, IoEvent) {
            ++calls; // Leave data unread: level triggering would dispatch it again.
            if (calls == 1) {
                EXPECT_TRUE(demuxer.add(sentinel.fd[0], IoEvent::Read, [&](int, IoEvent) {
                    demuxer.stop();
                }));
                EXPECT_EQ(::write(sentinel.fd[1], "s", 1), 1);
            }
        }, argument));
        // The explicit edge argument stays active across modify; mask-based ET
        // is explicitly supplied again, as in the previous API.
        ASSERT_TRUE(demuxer.modify(pair.fd[0], events));
        ASSERT_EQ(::write(pair.fd[1], "x", 1), 1);
        EXPECT_TRUE(run_to_stop(demuxer));
        EXPECT_EQ(calls, 1);
    }
}

TEST(EpollDemuxerBoundary, CallbackCanRemoveItselfAndStopSkipsRemainingBatch) {
    Pair pairs[2];
    EpollDemuxer demuxer(8);
    int calls = 0;
    for (auto& pair : pairs) {
        ASSERT_TRUE(demuxer.add(pair.fd[0], IoEvent::Read, [&](int fd, IoEvent) {
            ++calls;
            EXPECT_TRUE(demuxer.remove(fd));
            demuxer.stop();
        }));
        ASSERT_EQ(::write(pair.fd[1], "x", 1), 1);
    }
    EXPECT_TRUE(run_to_stop(demuxer));
    EXPECT_EQ(calls, 1);
}

TEST(EpollDemuxerBoundary, StopBeforeRunAndStartupStopRaceCannotRestartLoop) {
    EpollDemuxer stopped;
    stopped.stop();
    EXPECT_TRUE(run_to_stop(stopped));
    EXPECT_TRUE(run_to_stop(stopped));
    Pair pair;
    EXPECT_FALSE(stopped.add(pair.fd[0], IoEvent::Read, {}));
    EXPECT_EQ(errno, ECANCELED);
    for (int cycle = 0; cycle < 100; ++cycle) {
        EpollDemuxer demuxer;
        std::thread runner([&] { demuxer.run(); });
        demuxer.stop();
        runner.join();
    }
    // Exercise the service owner whose stop joins newly-created reactors.
    for (int cycle = 0; cycle < 20; ++cycle) {
        chwell::net::EpollTcpServer server(0, 2);
        ASSERT_TRUE(server.start_checked());
        server.stop();
        EXPECT_FALSE(server.start_checked());
    }
}

TEST(EpollDemuxerBoundary, CrossThreadWakeDoesNotStopOrInvokeCallbacks) {
    Pair pair;
    EpollDemuxer demuxer;
    std::promise<void> completed;
    auto done = completed.get_future();
    std::atomic<int> calls{0};
    ASSERT_TRUE(demuxer.add(pair.fd[0], IoEvent::Read, [&](int, IoEvent) {
        if (++calls == 1) completed.set_value();
        demuxer.stop();
    }));
    std::thread runner([&] { demuxer.run(); });
    for (int i = 0; i < 1000; ++i) EXPECT_TRUE(demuxer.wake());
    EXPECT_EQ(calls.load(), 0);
    EXPECT_EQ(::write(pair.fd[1], "x", 1), 1);
    const bool ready = done.wait_for(3s) == std::future_status::ready;
    demuxer.stop();
    runner.join();
    EXPECT_TRUE(ready);
    EXPECT_EQ(calls.load(), 1);
}

TEST(EpollDemuxerBoundary, ConcurrentRunnerIsRejectedAndExceptionReleasesRunnerSlot) {
    Pair pair;
    EpollDemuxer demuxer;
    std::promise<void> entered, release;
    auto started = entered.get_future(), gate = release.get_future();
    ASSERT_TRUE(demuxer.add(pair.fd[0], IoEvent::Read, [&](int, IoEvent) {
        entered.set_value(); gate.wait(); demuxer.stop();
    }));
    ASSERT_EQ(::write(pair.fd[1], "x", 1), 1);
    std::thread runner([&] { demuxer.run(); });
    const bool ready = started.wait_for(3s) == std::future_status::ready;
    if (ready) { demuxer.run(); EXPECT_EQ(errno, EBUSY); }
    release.set_value();
    demuxer.stop();
    runner.join();
    EXPECT_TRUE(ready);

    EpollDemuxer throwing;
    ASSERT_TRUE(throwing.add(pair.fd[0], IoEvent::Read, [](int, IoEvent) {
        throw std::runtime_error("callback failure");
    }));
    EXPECT_THROW(throwing.run(), std::runtime_error);
    ASSERT_TRUE(throwing.set_callback(pair.fd[0], [&](int, IoEvent) { throwing.stop(); }));
    EXPECT_TRUE(run_to_stop(throwing));
}

TEST(EpollDemuxerBoundary, CaptureDestructorsCanReenterRegistrationOperations) {
    Pair pair;
    EpollDemuxer demuxer;
    struct Capture {
        IoDemuxer& demuxer;
        ~Capture() { demuxer.wake(); demuxer.set_callback(-1, {}); }
    };
    auto capture = std::make_shared<Capture>(Capture{demuxer});
    ASSERT_TRUE(demuxer.add(pair.fd[0], IoEvent::Read, [capture](int, IoEvent) {}));
    capture.reset();
    EXPECT_TRUE(demuxer.set_callback(pair.fd[0], {}));
    capture = std::make_shared<Capture>(Capture{demuxer});
    EXPECT_TRUE(demuxer.set_callback(pair.fd[0], [capture](int, IoEvent) {}));
    capture.reset();
    EXPECT_TRUE(demuxer.remove(pair.fd[0]));
}

TEST(EpollDemuxerBoundary, ConnectionUsesInjectedReadinessBackendForReadAndWrite) {
    struct ManualDemuxer : IoDemuxer {
        EventCallback callback;
        IoEvent interest = IoEvent::None;
        int registered = -1;
        bool is_valid() const override { return true; }
        bool add(int fd, IoEvent events, EventCallback cb, bool) override {
            registered = fd; interest = events; callback = std::move(cb); return true;
        }
        bool modify(int, IoEvent events) override { interest = events; return true; }
        bool set_callback(int, EventCallback cb) override { callback = std::move(cb); return true; }
        bool remove(int) override { registered = -1; callback = {}; return true; }
        void run(int) override {}
        bool wake() override { return true; }
        void stop() override {}
        void dispatch(IoEvent events) { auto selected = callback; selected(registered, events); }
    } demuxer;
    Pair pair;
    auto connection = std::make_shared<chwell::net::EpollTcpConnection>(pair.take(0), &demuxer);
    std::string received;
    connection->set_message_callback([&](const auto&, std::string_view data) { received.append(data); });
    connection->send("hello");
    connection->start();
    EXPECT_TRUE(has_event(demuxer.interest, IoEvent::Write));
    demuxer.dispatch(IoEvent::Write);
    char buffer[5];
    ASSERT_EQ(::read(pair.fd[1], buffer, sizeof(buffer)), 5);
    EXPECT_EQ(std::string(buffer, sizeof(buffer)), "hello");
    EXPECT_FALSE(has_event(demuxer.interest, IoEvent::Write));
    ASSERT_EQ(::write(pair.fd[1], "ping", 4), 4);
    demuxer.dispatch(IoEvent::Read);
    EXPECT_EQ(received, "ping");
    demuxer.dispatch(IoEvent::Error);
    EXPECT_FALSE(connection->is_open());
    EXPECT_EQ(demuxer.registered, -1);
}
