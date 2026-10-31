// tools/mold_udp_demo.cpp
//
// The MoldUDP64 receiver over real UDP sockets on 127.0.0.1. One process, two threads:
//
//   sender thread    reads the file, packetizes it, passes each downstream packet through
//                    the deterministic injector (LossyChannel, drops only) and sendto()s the
//                    survivors to the receiver's feed socket. The same thread owns the
//                    retransmit server: it answers re-request datagrams arriving on its own
//                    socket by sendto()ing the replies back to the requester.
//   receiver thread  recvfrom()s the receiver socket (feed and retransmit answers arrive on
//                    it; the source port says which), drives the Receiver, and sendto()s its
//                    re-requests to the retransmit server's socket. The Receiver feeds the
//                    same BookBuilder sink the simulator uses.
//
//   mold_udp_demo <file> [--rate-mps N] [--drop p] [--seed s] [--retention n]
//                 [--dump-state path] [--dump-at-s S] [--json out] [--max-messages N]
//                 [--check-direct] [--rcvbuf bytes] [--tick-ms n] [--timeout-ticks n]
//                 [--max-attempts n] [--heartbeat-every n] [--max-live-orders n]
//                 [--idle-timeout-ms n] [--end-wait-ms n]
//
//   --rate-mps N         pace the sender to N messages per second (default 1,000,000; 0 = as
//                        fast as it goes, which on a full day floods the receiver socket and
//                        outruns the recovery path)
//   --drop p             INJECTED downstream drop probability per packet (default 0)
//   --retention n        messages the retransmit server remembers (default 1,048,576)
//   --rcvbuf bytes       requested SO_RCVBUF of the receiver socket (default 8 MiB; the kernel
//                        may grant less, the granted size is reported)
//   --tick-ms n          wall milliseconds per Receiver tick (default 5)
//   --idle-timeout-ms n  the receiver gives up after this long with no datagram (default 5000)
//   --end-wait-ms n      the sender stops re-sending end of session after this long (default 10000)
//
// Two kinds of loss are reported separately and never mixed. INJECTED drops are the packets
// the injector withheld on purpose (channel counts). KERNEL drops are datagrams a send call
// handed to the kernel that never reached the receiving socket (the kernel discards when a
// socket buffer is full); they are measured as datagrams sent minus datagrams received, after
// both threads have stopped, and can be nonzero at --drop 0. The receiver recovers both the
// same way, by re-request.
//
// This is a loopback demonstration of the protocol over real sockets: not multicast, one host,
// and not a latency claim. Nothing here measures how long a message takes.
//
// Exit codes (shared by every tool): 0 success, 2 input error, 3 the run failed: the receiver
// went Stale or gave up, the delivered stream differs from the file's, or the state differs
// from the direct replay. Every wait has a timeout and both threads are joined on every path.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <itch/mapped_file.hpp>
#include <itch/provenance.hpp>
#include <itch/wire.hpp>
#include <memory>
#include <mold/cli.hpp>
#include <mold/sim.hpp>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace mold::cli;
using Clock = std::chrono::steady_clock;

// The run itself could not be carried out (a socket call failed, the books of datagrams do not
// add up): exit 3, not an input error.
struct RunFailed : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options : CommonOptions {
    mold::SimConfig sim;
    std::uint64_t   rate_mps    = 1'000'000;
    std::uint32_t   tick_ms     = 5;
    std::uint64_t   rcvbuf      = 8U << 20;
    std::uint64_t   idle_ms     = 5000;
    std::uint64_t   end_wait_ms = 10000;
};

void usage() {
    std::fprintf(stderr,
                 "usage: mold_udp_demo <file> [--rate-mps N] [--drop p] [--seed s]\n"
                 "                     [--retention n] [--dump-state path] [--dump-at-s S]\n"
                 "                     [--json out] [--max-messages N] [--check-direct]\n"
                 "                     [--rcvbuf bytes] [--tick-ms n] [--timeout-ticks n]\n"
                 "                     [--max-attempts n] [--heartbeat-every n]\n"
                 "                     [--max-live-orders n] [--idle-timeout-ms n]\n"
                 "                     [--end-wait-ms n]\n");
}

Options parse(int argc, char** argv) {
    Options o;
    // Over real sockets a re-request takes real time, so the defaults wait longer than the
    // simulator's: 20 ticks of 5 ms before asking again, 16 asks before giving up.
    o.sim.rec.timeout_ticks = 20;
    for (int i = 1; i < argc; ++i) {
        const std::string a     = argv[i];
        auto              value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw InputError(a + " needs a value");
            }
            return argv[++i];
        };
        auto& rec = o.sim.rec;
        if (a == "--drop") {
            o.sim.channel.loss = parse_prob(a, value());
        } else if (a == "--seed") {
            o.sim.channel.seed = parse_u64(a, value());
        } else if (a == "--rate-mps") {
            o.rate_mps = parse_u64(a, value());
        } else if (a == "--retention") {
            o.sim.retention = static_cast<std::size_t>(parse_u64(a, value()));
            if (o.sim.retention == 0) {
                throw InputError("--retention must be at least 1");
            }
        } else if (a == "--rcvbuf") {
            o.rcvbuf = parse_u64(a, value());
            if (o.rcvbuf == 0 || o.rcvbuf > (1U << 30)) {
                throw InputError("--rcvbuf must be between 1 and 1073741824");
            }
        } else if (a == "--tick-ms") {
            o.tick_ms = parse_u32(a, value());
            if (o.tick_ms == 0) {
                throw InputError("--tick-ms must be at least 1");
            }
        } else if (a == "--timeout-ticks") {
            rec.timeout_ticks = parse_u32(a, value());
            if (rec.timeout_ticks == 0) {
                throw InputError("--timeout-ticks must be at least 1");
            }
        } else if (a == "--max-attempts") {
            rec.max_attempts = parse_u32(a, value());
            if (rec.max_attempts == 0) {
                throw InputError("--max-attempts must be at least 1");
            }
        } else if (a == "--heartbeat-every") {
            o.sim.heartbeat_every = parse_u32(a, value());
        } else if (a == "--idle-timeout-ms") {
            o.idle_ms = parse_u64(a, value());
            if (o.idle_ms == 0) {
                throw InputError("--idle-timeout-ms must be at least 1");
            }
        } else if (a == "--end-wait-ms") {
            o.end_wait_ms = parse_u64(a, value());
            if (o.end_wait_ms == 0) {
                throw InputError("--end-wait-ms must be at least 1");
            }
        } else {
            parse_common(a, value, o, o.sim);
        }
    }
    finish_common(o, o.sim);
    return o;
}

// ---- sockets ----------------------------------------------------------------------------

class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    Socket(const Socket&)            = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) {
            close();
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    ~Socket() { close(); }
    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    int fd_ = -1;
};

[[nodiscard]] std::string errno_text(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

[[nodiscard]] sockaddr_in loopback(std::uint16_t port_net_order) {
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = port_net_order;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

[[nodiscard]] Socket make_udp() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        throw RunFailed(errno_text("socket"));
    }
    return Socket(fd);
}

// Binds to 127.0.0.1 on a port the kernel picks, and returns that address.
[[nodiscard]] sockaddr_in bind_loopback(const Socket& s) {
    sockaddr_in a = loopback(0);
    if (::bind(s.fd(), reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        throw RunFailed(errno_text("bind"));
    }
    socklen_t len = sizeof a;
    if (::getsockname(s.fd(), reinterpret_cast<sockaddr*>(&a), &len) != 0) {
        throw RunFailed(errno_text("getsockname"));
    }
    return a;
}

void set_timeout(const Socket& s, int opt, std::uint32_t ms) {
    timeval tv{};
    tv.tv_sec  = static_cast<decltype(tv.tv_sec)>(ms / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((ms % 1000) * 1000);
    if (::setsockopt(s.fd(), SOL_SOCKET, opt, &tv, sizeof tv) != 0) {
        throw RunFailed(errno_text("setsockopt timeout"));
    }
}

// Asks for `bytes`, halving until the kernel accepts, and returns what it granted.
std::uint64_t set_buffer(const Socket& s, int opt, std::uint64_t bytes) {
    for (std::uint64_t want = bytes; want >= (64U << 10); want /= 2) {
        const int v = static_cast<int>(want);
        if (::setsockopt(s.fd(), SOL_SOCKET, opt, &v, sizeof v) == 0) {
            break;
        }
    }
    int       got = 0;
    socklen_t len = sizeof got;
    if (::getsockopt(s.fd(), SOL_SOCKET, opt, &got, &len) != 0) {
        throw RunFailed(errno_text("getsockopt buffer"));
    }
    return static_cast<std::uint64_t>(got);
}

// One datagram out. True when the kernel took it whole.
bool send_datagram(int fd, const sockaddr_in& to, std::span<const std::byte> pkt) {
    const ssize_t n =
        ::sendto(fd, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    return n == static_cast<ssize_t>(pkt.size());
}

// ---- the two threads --------------------------------------------------------------------

struct Control {
    std::atomic<bool> rx_done{false};  // the receiver thread has stopped
    std::atomic<bool> abort{false};    // a thread failed: everyone stop
};

// The sender thread's counts. Written only by that thread; read after it is joined.
struct SenderStats {
    std::uint64_t       feed_sent         = 0;  // datagrams the kernel accepted
    std::uint64_t       feed_errors       = 0;  // sendto failed (counted, not retried)
    std::uint64_t       retransmit_sent   = 0;
    std::uint64_t       retransmit_errors = 0;
    std::uint64_t       requests_received = 0;
    std::uint64_t       requests_ignored  = 0;  // wrong size, session, or an empty range
    std::uint64_t       source_packets    = 0;
    std::uint64_t       source_messages   = 0;
    std::uint64_t       end_sends         = 0;
    mold::ChannelCounts channel;
};

// Replies of the retransmit server go back to whoever asked, from the server's own socket.
class ReplySink final : public mold::PacketSink {
public:
    ReplySink(int fd, SenderStats& st) : fd_(fd), st_(st) {}
    void aim(const sockaddr_in& to) { to_ = to; }
    void send(std::span<const std::byte> pkt) override {
        if (send_datagram(fd_, to_, pkt)) {
            ++st_.retransmit_sent;
        } else {
            ++st_.retransmit_errors;
        }
    }

private:
    int          fd_;
    SenderStats& st_;
    sockaddr_in  to_{};
};

class SenderThread {
public:
    SenderThread(const Options& o, std::span<const std::byte> source, int feed_fd, int server_fd,
                 const sockaddr_in& rx_addr, Control& ctl)
        : o_(o),
          source_(source),
          feed_fd_(feed_fd),
          server_fd_(server_fd),
          rx_addr_(rx_addr),
          ctl_(ctl) {}

    void run() {
        try {
            body();
        } catch (...) {
            error_ = std::current_exception();
            ctl_.abort.store(true);
        }
    }
    [[nodiscard]] const SenderStats&        stats() const noexcept { return st_; }
    [[nodiscard]] const std::exception_ptr& error() const noexcept { return error_; }

private:
    void body() {
        const mold::SimConfig& cfg = o_.sim;
        mold::Packetizer       pk(cfg.session, itch::Framer(source_), cfg.mtu);
        mold::RetransmitServer server(cfg.session, cfg.retention, cfg.mtu);
        mold::LossyChannel     channel(cfg.channel);
        ReplySink              reply(server_fd_, st_);
        std::vector<std::byte> buf;

        const auto drain = [&] {
            while (channel.poll(buf)) {
                if (send_datagram(feed_fd_, rx_addr_, buf)) {
                    ++st_.feed_sent;
                } else {
                    ++st_.feed_errors;
                }
            }
        };

        const auto t0 = Clock::now();
        while (!ctl_.abort.load() && !ctl_.rx_done.load()) {
            const auto pkt = pk.next();
            if (!pkt) {
                break;
            }
            const auto          msgs = pk.last_messages();
            const std::uint64_t seq0 = pk.next_seq() - msgs.size();
            for (std::size_t i = 0; i < msgs.size(); ++i) {
                server.retain(seq0 + i, msgs[i]);
            }
            channel.send(*pkt);
            ++st_.source_packets;
            if (cfg.heartbeat_every != 0 && st_.source_packets % cfg.heartbeat_every == 0) {
                channel.send(pk.heartbeat());
            }
            drain();
            serve_requests(server, reply);
            if (o_.rate_mps != 0) {
                // The time by which the messages sent so far are due at this rate.
                const double due_s =
                    static_cast<double>(pk.next_seq() - 1) / static_cast<double>(o_.rate_mps);
                wait_until(t0 + std::chrono::duration_cast<Clock::duration>(
                                    std::chrono::duration<double>(due_s)),
                           server, reply);
            }
        }
        st_.source_messages = pk.next_seq() - 1;
        if (!ctl_.abort.load() && !ctl_.rx_done.load() && pk.status() != itch::FrameStatus::End) {
            throw std::runtime_error("framing error in the source: status " +
                                     std::to_string(static_cast<int>(pk.status())));
        }

        // End of session, re-sent every tick (it can be dropped too) while requests are still
        // served, until the receiver stops or the wait runs out.
        const auto deadline = Clock::now() + std::chrono::milliseconds(o_.end_wait_ms);
        while (!ctl_.abort.load() && !ctl_.rx_done.load() && Clock::now() < deadline) {
            channel.send(pk.end_of_session());
            channel.finish();
            ++st_.end_sends;
            drain();
            wait_until(Clock::now() + std::chrono::milliseconds(o_.tick_ms), server, reply);
        }
        channel.finish();
        drain();
        st_.channel = channel.counts();
    }

    // Answers every re-request already waiting on the server socket.
    void serve_requests(mold::RetransmitServer& server, ReplySink& reply) {
        std::array<std::byte, 2048> in;
        for (;;) {
            sockaddr_in from{};
            socklen_t   flen = sizeof from;
            const auto  n    = ::recvfrom(server_fd_, in.data(), in.size(), MSG_DONTWAIT,
                                          reinterpret_cast<sockaddr*>(&from), &flen);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return;  // EAGAIN: nothing waiting
            }
            ++st_.requests_received;
            if (static_cast<std::size_t>(n) != mold::kHeaderSize ||
                std::memcmp(in.data(), o_.sim.session.data(), o_.sim.session.size()) != 0) {
                ++st_.requests_ignored;
                continue;
            }
            const std::uint64_t seq   = itch::load_be64(in.data() + 10);
            const std::uint16_t count = itch::load_be16(in.data() + 18);
            if (seq == 0 || count == 0 || count == mold::kEndOfSession) {
                ++st_.requests_ignored;
                continue;
            }
            reply.aim(from);
            (void)server.serve(seq, count, reply);
        }
    }

    // Serves requests until `deadline`, sleeping in 1 ms polls and spinning the last stretch.
    void wait_until(Clock::time_point deadline, mold::RetransmitServer& server, ReplySink& reply) {
        for (;;) {
            serve_requests(server, reply);
            if (ctl_.abort.load() || ctl_.rx_done.load()) {
                return;
            }
            const auto now = Clock::now();
            if (now >= deadline) {
                return;
            }
            if (deadline - now > std::chrono::microseconds(1500)) {
                pollfd p{server_fd_, POLLIN, 0};
                (void)::poll(&p, 1, 1);
            }
        }
    }

    const Options&             o_;
    std::span<const std::byte> source_;
    int                        feed_fd_;
    int                        server_fd_;
    sockaddr_in                rx_addr_;
    Control&                   ctl_;
    SenderStats                st_;
    std::exception_ptr         error_;
};

struct ReceiverStats {
    std::uint64_t feed_received       = 0;
    std::uint64_t retransmit_received = 0;
    std::uint64_t foreign             = 0;  // from neither of our sender sockets
    std::uint64_t requests_sent       = 0;
    std::uint64_t request_errors      = 0;
};

// The Receiver's Requester: a 20-byte request datagram to the retransmit server's socket.
struct UdpRequester {
    int            fd;
    sockaddr_in    server;
    mold::Session  session;
    ReceiverStats* st;
    void           request(std::uint64_t seq, std::uint16_t count) {
        std::array<std::byte, mold::kHeaderSize> pkt{};
        mold::write_header(pkt.data(), session, seq, count);
        if (send_datagram(fd, server, pkt)) {
            ++st->requests_sent;
        } else {
            ++st->request_errors;
        }
    }
};

enum class GaveUp { None, IdleTimeout, Aborted };

const char* to_string(GaveUp g) noexcept {
    switch (g) {
        case GaveUp::None:
            return "none";
        case GaveUp::IdleTimeout:
            return "idle_timeout";
        case GaveUp::Aborted:
            return "aborted";
    }
    return "unknown";
}

class ReceiverThread {
public:
    ReceiverThread(const Options& o, mold::ReplaySink& sink, int rx_fd,
                   const sockaddr_in& server_addr, Control& ctl)
        : o_(o), sink_(sink), rx_fd_(rx_fd), server_addr_(server_addr), ctl_(ctl) {}

    void run(std::uint16_t server_port_net) {
        try {
            body(server_port_net);
        } catch (...) {
            error_ = std::current_exception();
            ctl_.abort.store(true);
        }
        ctl_.rx_done.store(true);
    }

    [[nodiscard]] const ReceiverStats&      stats() const noexcept { return st_; }
    [[nodiscard]] mold::RxState             state() const noexcept { return state_; }
    [[nodiscard]] mold::StaleReason         stale() const noexcept { return stale_; }
    [[nodiscard]] const mold::RxCounts&     counts() const noexcept { return counts_; }
    [[nodiscard]] GaveUp                    gave_up() const noexcept { return gave_up_; }
    [[nodiscard]] const std::exception_ptr& error() const noexcept { return error_; }

private:
    void body(std::uint16_t server_port_net) {
        UdpRequester requester{rx_fd_, server_addr_, o_.sim.session, &st_};
        mold::Receiver<mold::ReplaySink, UdpRequester> rx(o_.sim.rec, sink_, requester);

        std::array<std::byte, 2048> buf;
        const auto                  tick          = std::chrono::milliseconds(o_.tick_ms);
        const auto                  idle          = std::chrono::milliseconds(o_.idle_ms);
        auto                        last_tick     = Clock::now();
        auto                        last_activity = last_tick;
        while (rx.state() != mold::RxState::Finished && rx.state() != mold::RxState::Stale) {
            sockaddr_in from{};
            socklen_t   flen = sizeof from;
            const auto  n    = ::recvfrom(rx_fd_, buf.data(), buf.size(), 0,
                                          reinterpret_cast<sockaddr*>(&from), &flen);
            auto        now  = Clock::now();
            if (n >= 0) {
                last_activity = now;
                const std::span<const std::byte> pkt(buf.data(), static_cast<std::size_t>(n));
                if (from.sin_port == server_port_net) {
                    ++st_.retransmit_received;
                    rx.on_packet(pkt, true);
                } else if (from.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
                    ++st_.feed_received;
                    rx.on_packet(pkt, false);
                } else {
                    ++st_.foreign;
                }
                now = Clock::now();
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                throw RunFailed(errno_text("recvfrom"));
            }
            if (now - last_tick >= tick) {
                rx.on_tick();  // one tick per elapsed period, however late the loop is
                last_tick = now;
            }
            if (ctl_.abort.load()) {
                gave_up_ = GaveUp::Aborted;
                break;
            }
            if (now - last_activity > idle) {
                gave_up_ = GaveUp::IdleTimeout;
                break;
            }
        }
        state_  = rx.state();
        stale_  = rx.stale_reason();
        counts_ = rx.counts();
    }

    const Options&     o_;
    mold::ReplaySink&  sink_;
    int                rx_fd_;
    sockaddr_in        server_addr_;
    Control&           ctl_;
    ReceiverStats      st_;
    mold::RxState      state_ = mold::RxState::Live;
    mold::StaleReason  stale_ = mold::StaleReason::None;
    mold::RxCounts     counts_;
    GaveUp             gave_up_ = GaveUp::None;
    std::exception_ptr error_;
};

// Joins on every path out of run(), including an exception.
struct Joiner {
    std::thread& t;
    Control&     ctl;
    ~Joiner() {
        if (t.joinable()) {
            ctl.abort.store(true);
            t.join();
        }
    }
};

// Counts the datagrams still sitting in a socket, waiting 50 ms for stragglers still in the
// loopback interface queue. `classify(from)` is called per datagram.
template <class F>
void drain_socket(int fd, F&& classify) {
    std::array<std::byte, 2048> buf;
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 50) <= 0) {
            return;
        }
        sockaddr_in from{};
        socklen_t   flen = sizeof from;
        const auto  n    = ::recvfrom(fd, buf.data(), buf.size(), MSG_DONTWAIT,
                                      reinterpret_cast<sockaddr*>(&from), &flen);
        if (n >= 0) {
            classify(from);
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return;
        }
    }
}

// ---- report -----------------------------------------------------------------------------

struct Run {
    mold::RxState       state   = mold::RxState::Live;
    mold::StaleReason   stale   = mold::StaleReason::None;
    GaveUp              gave_up = GaveUp::None;
    mold::RxCounts      rx;
    SenderStats         tx;
    ReceiverStats       rxs;
    std::uint64_t       requests_received_total = 0;  // incl. those found at the end
    std::uint64_t       feed_received_total     = 0;
    std::uint64_t       retx_received_total     = 0;
    std::uint64_t       rcvbuf_granted          = 0;
    mold::ReplayOutcome out;
    double              seconds = 0;
};

struct Kernel {
    std::int64_t  feed_lost    = 0;
    std::int64_t  retx_lost    = 0;
    std::int64_t  request_lost = 0;
    std::uint64_t send_errors  = 0;
};

void write_json(const Options& o, const itch::MappedFile& mf, std::size_t used_bytes,
                std::uint64_t source_messages, const std::string& source_sha, const Run& r,
                const Kernel& k, const mold::ReplayOutcome* direct, const Verdict& v,
                const std::string& dump_file) {
    std::filesystem::path out(o.json_path);
    if (out.has_parent_path()) {
        std::filesystem::create_directories(out.parent_path());
    }
    std::ofstream js(out);
    if (!js) {
        throw InputError("cannot write '" + o.json_path + "'");
    }
    const auto& rec = o.sim.rec;
    const auto& rx  = r.rx;
    const auto& cc  = r.tx.channel;
    js << std::setprecision(10);
    const std::string manifest = "\"data_manifest\": " + manifest_json(o.file);
    js << "{\n  \"meta\": " << itch::provenance_json(itch::make_provenance(o.file), 2, manifest)
       << ",\n";
    js << "  \"note\": \"loopback UDP on one host; not multicast; not a latency claim\",\n";
    js << "  \"config\": {\n";
    js << "    \"transport\": \"UDP over 127.0.0.1, two threads in one process\",\n";
    js << "    \"seed\": " << o.sim.channel.seed << ",\n";
    js << "    \"drop\": " << o.sim.channel.loss << ",\n";
    js << "    \"rate_mps\": " << o.rate_mps << ",\n";
    js << "    \"retention_messages\": " << o.sim.retention << ",\n";
    js << "    \"tick_ms\": " << o.tick_ms << ",\n";
    js << "    \"timeout_ticks\": " << rec.timeout_ticks << ",\n";
    js << "    \"max_attempts\": " << rec.max_attempts << ",\n";
    js << "    \"max_buffered_packets\": " << rec.max_buffered_packets << ",\n";
    js << "    \"max_gap_messages\": " << rec.max_gap_messages << ",\n";
    js << "    \"heartbeat_every_packets\": " << o.sim.heartbeat_every << ",\n";
    js << "    \"mtu\": " << o.sim.mtu << ",\n";
    js << "    \"idle_timeout_ms\": " << o.idle_ms << ",\n";
    js << "    \"end_wait_ms\": " << o.end_wait_ms << ",\n";
    js << "    \"max_messages\": " << o.max_messages << ",\n";
    js << "    \"dump_at_s\": " << (o.sim.dump.at_set ? std::to_string(o.sim.dump.at_s) : "null")
       << ",\n";
    js << "    \"check_direct\": " << tf(o.check_direct) << "\n  },\n";
    js << "  \"source\": {\"file\": " << json_str(std::filesystem::path(o.file).filename().string())
       << ", \"file_bytes\": " << mf.bytes().size() << ", \"bytes_used\": " << used_bytes
       << ", \"messages\": " << source_messages << ", \"packets\": " << r.tx.source_packets
       << ", \"stream_sha256\": " << json_str(source_sha) << "},\n";
    js << "  \"result\": {\"state\": " << json_str(mold::to_string(r.state))
       << ", \"stale_reason\": " << json_str(mold::to_string(r.stale))
       << ", \"gave_up\": " << json_str(to_string(r.gave_up))
       << ", \"finished\": " << tf(v.finished) << ", \"delivered_messages\": " << r.out.delivered
       << ", \"stream_sha256\": " << json_str(r.out.stream_sha256)
       << ", \"stream_matches_source\": " << tf(v.stream_matches_source)
       << ", \"end_of_session_sends\": " << r.tx.end_sends << ", \"handled\": " << r.out.handled
       << ", \"skipped\": " << r.out.skipped << ", \"wrong_length\": " << r.out.wrong_length
       << ", \"live_orders_at_end\": " << r.out.live_orders << ", \"seconds\": " << std::fixed
       << std::setprecision(1) << r.seconds << std::defaultfloat << std::setprecision(10) << "},\n";
    js << "  \"rx\": {\"gaps_detected\": " << rx.gaps_detected
       << ", \"dup_packets\": " << rx.dup_packets
       << ", \"partial_overlaps\": " << rx.partial_overlaps
       << ", \"wrong_session\": " << rx.wrong_session << ", \"malformed\": " << rx.malformed
       << ", \"retransmit_requests\": " << rx.retransmit_requests
       << ", \"messages_delivered_late\": " << rx.messages_delivered_late
       << ", \"messages_from_retransmit\": " << rx.messages_from_retransmit
       << ", \"messages_buffered_peak\": " << rx.messages_buffered_peak
       << ", \"buffer_overflow_drops\": " << rx.buffer_overflow_drops
       << ", \"max_gap_len\": " << rx.max_gap_len << "},\n";
    // Packets the injector withheld on purpose. `delivered_to_socket` is what it let through
    // to the sender's sendto loop.
    js << "  \"injected\": {\"packets_offered\": " << cc.sent
       << ", \"downstream_dropped\": " << cc.downstream_dropped
       << ", \"messages_lost_downstream\": " << cc.messages_lost_downstream
       << ", \"delivered_to_socket\": " << cc.delivered << "},\n";
    js << "  \"sockets\": {\"rcvbuf_bytes\": " << r.rcvbuf_granted
       << ", \"rcvbuf_requested\": " << o.rcvbuf << ", \"feed_sent\": " << r.tx.feed_sent
       << ", \"feed_received\": " << r.feed_received_total
       << ", \"retransmit_sent\": " << r.tx.retransmit_sent
       << ", \"retransmit_received\": " << r.retx_received_total
       << ", \"requests_sent\": " << r.rxs.requests_sent
       << ", \"requests_received\": " << r.requests_received_total
       << ", \"requests_ignored\": " << r.tx.requests_ignored
       << ", \"foreign_datagrams\": " << r.rxs.foreign << "},\n";
    // Datagrams the kernel took and never delivered to the receiving socket (sent - received).
    js << "  \"kernel\": {\"feed_datagrams_lost\": " << k.feed_lost
       << ", \"retransmit_datagrams_lost\": " << k.retx_lost
       << ", \"request_datagrams_lost\": " << k.request_lost
       << ", \"send_errors\": " << k.send_errors << "},\n";
    js << "  \"dump\": ";
    if (!o.sim.dump.enabled) {
        js << "null";
    } else {
        const auto& d = r.out.dump;
        js << "{\"path\": " << json_str(dump_file) << ", \"at_end\": " << tf(d.at_end)
           << ", \"at_second\": " << d.at_second << ", \"at_message\": " << d.at_message
           << ", \"bytes\": " << d.bytes << ", \"resting_orders\": " << d.orders()
           << ", \"non_empty\": " << tf(d.bytes > 0) << ", \"complete\": " << tf(d.complete)
           << ", \"valid\": " << tf(v.exit_code == kExitOk)
           << ", \"sha256\": " << json_str(d.sha256);
        if (direct != nullptr) {
            js << ", \"direct_sha256\": " << json_str(direct->dump.sha256)
               << ", \"direct_bytes\": " << direct->dump.bytes
               << ", \"matches_direct\": " << tf(v.state_matches_direct == 1);
        }
        js << "}";
    }
    js << ",\n  \"verdict\": {\"passed\": " << tf(v.exit_code == kExitOk)
       << ", \"exit_code\": " << v.exit_code << "}\n}\n";
    js.close();
    if (!js) {
        throw InputError("failed writing '" + o.json_path + "'");
    }
}

using ull = unsigned long long;

int run(const Options& o) {
    const auto                 mf     = itch::MappedFile::open(o.file);
    std::span<const std::byte> source = mf.bytes();
    if (o.max_messages != 0) {
        source = source.first(mold::prefix_bytes(source, o.max_messages));
    }
    const mold::SourceSummary summary = mold::summarize_source(source);
    if (summary.status != itch::FrameStatus::End) {
        throw InputError("framing error at offset " + std::to_string(summary.offset) + " of " +
                         std::to_string(source.size()) +
                         " bytes; the file is truncated or corrupt");
    }
    const std::uint64_t source_messages = summary.messages;
    const std::string&  source_sha      = summary.sha256;

    std::unique_ptr<mold::ReplayOutcome> direct;
    if (o.check_direct) {
        direct = std::make_unique<mold::ReplayOutcome>(
            mold::direct_replay(source, o.sim.max_live_orders, o.sim.dump));
    }

    // Three sockets: the sender's feed socket, the retransmit server's socket, and the
    // receiver's socket (feed and retransmit answers in, re-requests out).
    Socket            feed_tx     = make_udp();
    Socket            server      = make_udp();
    Socket            rxsock      = make_udp();
    const sockaddr_in server_addr = bind_loopback(server);
    const sockaddr_in rx_addr     = bind_loopback(rxsock);
    (void)bind_loopback(feed_tx);  // so its datagrams carry a stable loopback source
    Run r;
    r.rcvbuf_granted = set_buffer(rxsock, SO_RCVBUF, o.rcvbuf);
    (void)set_buffer(server, SO_RCVBUF, 1U << 20);
    (void)set_buffer(feed_tx, SO_SNDBUF, 1U << 20);
    (void)set_buffer(server, SO_SNDBUF, 1U << 20);
    // No call blocks for long, whatever happens to the other thread.
    set_timeout(rxsock, SO_RCVTIMEO, 1);
    set_timeout(rxsock, SO_SNDTIMEO, 100);
    set_timeout(feed_tx, SO_SNDTIMEO, 100);
    set_timeout(server, SO_SNDTIMEO, 100);

    mold::ReplaySink sink(o.sim.max_live_orders, o.sim.dump);
    Control          ctl;
    SenderThread     sender(o, source, feed_tx.fd(), server.fd(), rx_addr, ctl);
    ReceiverThread   receiver(o, sink, rxsock.fd(), server_addr, ctl);

    const auto t0 = Clock::now();
    {
        std::thread rx_thread([&] { receiver.run(server_addr.sin_port); });
        Joiner      rx_join{rx_thread, ctl};
        std::thread tx_thread([&] { sender.run(); });
        Joiner      tx_join{tx_thread, ctl};
        tx_thread.join();  // the sender ends on its own: the file, then a bounded end phase
        rx_thread.join();  // the receiver ends on Finished, Stale, idle, or abort
    }
    r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();

    if (sender.error()) {
        std::rethrow_exception(sender.error());
    }
    if (receiver.error()) {
        std::rethrow_exception(receiver.error());
    }

    // Both threads are stopped. Whatever is still queued in a socket arrived (it was not
    // lost); it only was never read.
    r.tx                      = sender.stats();
    r.rxs                     = receiver.stats();
    r.feed_received_total     = r.rxs.feed_received;
    r.retx_received_total     = r.rxs.retransmit_received;
    r.requests_received_total = r.tx.requests_received;
    drain_socket(rxsock.fd(), [&](const sockaddr_in& from) {
        if (from.sin_port == server_addr.sin_port) {
            ++r.retx_received_total;
        } else {
            ++r.feed_received_total;
        }
    });
    drain_socket(server.fd(), [&](const sockaddr_in&) { ++r.requests_received_total; });

    Kernel k;
    k.feed_lost = static_cast<std::int64_t>(r.tx.feed_sent) -
                  static_cast<std::int64_t>(r.feed_received_total);
    k.retx_lost = static_cast<std::int64_t>(r.tx.retransmit_sent) -
                  static_cast<std::int64_t>(r.retx_received_total);
    k.request_lost = static_cast<std::int64_t>(r.rxs.requests_sent) -
                     static_cast<std::int64_t>(r.requests_received_total);
    k.send_errors = r.tx.feed_errors + r.tx.retransmit_errors + r.rxs.request_errors;
    if (k.feed_lost < 0 || k.retx_lost < 0 || k.request_lost < 0 || r.rxs.foreign != 0) {
        // More received than sent means the accounting (or a stray sender) is wrong.
        throw RunFailed("datagram accounting is inconsistent: received more than sent");
    }

    r.state   = receiver.state();
    r.stale   = receiver.stale();
    r.gave_up = receiver.gave_up();
    r.rx      = receiver.counts();
    r.out     = sink.finish(r.state == mold::RxState::Finished);

    const Verdict v =
        judge(r.state == mold::RxState::Finished, r.out, source_sha, source_messages, direct.get());
    const bool ok = v.exit_code == kExitOk;

    std::string dump_file = o.dump_path;
    if (o.sim.dump.enabled) {
        dump_file = store_dump(o.dump_path, r.out.dump.data, ok);
    }
    if (!o.json_path.empty()) {
        write_json(o, mf, source.size(), source_messages, source_sha, r, k, direct.get(), v,
                   dump_file);
    }

    const auto& cc = r.tx.channel;
    std::printf(
        "%s: %llu messages in %llu packets, %.1f s (loopback UDP; not a latency measurement)\n"
        "injected: offered %llu, dropped %llu downstream (%llu messages)\n"
        "kernel: feed datagrams lost %lld of %llu sent, retransmit lost %lld of %llu, "
        "requests lost %lld of %llu, send errors %llu (rcvbuf %llu bytes)\n"
        "receiver: %s (%s)%s, gaps %llu, requests %llu, from retransmit %llu, dup %llu, "
        "overlap %llu, buffer peak %llu msgs, overflow drops %llu, max gap %llu\n"
        "stream: %s (%llu delivered)\n",
        o.file.c_str(), static_cast<ull>(source_messages), static_cast<ull>(r.tx.source_packets),
        r.seconds, static_cast<ull>(cc.sent), static_cast<ull>(cc.downstream_dropped),
        static_cast<ull>(cc.messages_lost_downstream), static_cast<long long>(k.feed_lost),
        static_cast<ull>(r.tx.feed_sent), static_cast<long long>(k.retx_lost),
        static_cast<ull>(r.tx.retransmit_sent), static_cast<long long>(k.request_lost),
        static_cast<ull>(r.rxs.requests_sent), static_cast<ull>(k.send_errors),
        static_cast<ull>(r.rcvbuf_granted), mold::to_string(r.state), mold::to_string(r.stale),
        r.gave_up == GaveUp::None ? "" : (std::string(" gave up: ") + to_string(r.gave_up)).c_str(),
        static_cast<ull>(r.rx.gaps_detected), static_cast<ull>(r.rx.retransmit_requests),
        static_cast<ull>(r.rx.messages_from_retransmit), static_cast<ull>(r.rx.dup_packets),
        static_cast<ull>(r.rx.partial_overlaps), static_cast<ull>(r.rx.messages_buffered_peak),
        static_cast<ull>(r.rx.buffer_overflow_drops), static_cast<ull>(r.rx.max_gap_len),
        v.stream_matches_source ? "IDENTICAL to the file's" : "DIFFERS from the file's",
        static_cast<ull>(r.out.delivered));
    if (o.sim.dump.enabled) {
        std::printf("state: %llu resting orders (%llu bytes) at %s -> %s%s\n",
                    static_cast<ull>(r.out.dump.orders()), static_cast<ull>(r.out.dump.bytes),
                    r.out.dump.at_end
                        ? "end of run"
                        : ("feed second " + std::to_string(r.out.dump.at_second)).c_str(),
                    dump_file.c_str(), v.finished ? "" : " (INCOMPLETE)");
        if (direct) {
            std::printf("state vs direct replay: %s (%llu resting orders direct)\n",
                        v.state_matches_direct == 1 ? "IDENTICAL" : "DIFFERS",
                        static_cast<ull>(direct->dump.orders()));
        }
    }
    if (!v.finished) {
        std::fprintf(stderr,
                     "mold_udp_demo: FAILED: receiver ended %s (%s%s%s); books are incomplete\n",
                     mold::to_string(r.state), mold::to_string(r.stale),
                     r.gave_up == GaveUp::None ? "" : ", gave up: ",
                     r.gave_up == GaveUp::None ? "" : to_string(r.gave_up));
    } else if (const std::string why = failure_after_finish(v); !why.empty()) {
        std::fprintf(stderr, "mold_udp_demo: FAILED: %s\n", why.c_str());
    }
    return v.exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    try {
        o = parse(argc, argv);
    } catch (const InputError& e) {
        std::fprintf(stderr, "mold_udp_demo: %s\n", e.what());
        usage();
        return kExitInput;
    }
    try {
        return run(o);
    } catch (const RunFailed& e) {
        std::fprintf(stderr, "mold_udp_demo: FAILED: %s\n", e.what());
        return kExitFailed;
    } catch (const book::CapacityExceeded& e) {
        std::fprintf(stderr, "mold_udp_demo: order capacity exhausted: %s\n", e.what());
        return kExitInput;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mold_udp_demo: %s\n", e.what());
        return kExitInput;
    }
}
