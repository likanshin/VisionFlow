#include "visionflow/net/server.hpp"
#include "visionflow/concurrency/thread_pool.hpp"
#include "visionflow/core/fd.hpp"
#include "visionflow/core/logger.hpp"
#include "visionflow/protocol/protocol.hpp"
#include "visionflow/vision/processor.hpp"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <signal.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace visionflow {
namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;
double milliseconds(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
void checked(int result, const char* operation) {
    if (result < 0) throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
struct Job {
    std::uint64_t session = 0, id = 0;
    Bytes image;
    Clock::time_point queued;
};
struct Completion { std::uint64_t session = 0, id = 0; Bytes packet; bool error = false; };
struct Output { std::uint64_t id; Bytes packet; std::size_t offset = 0; };
struct Connection {
    UniqueFd socket;
    bool close_requested = false;
    std::array<std::uint8_t, protocol::header_size> header{};
    std::size_t header_used = 0, body_used = 0, send_bytes = 0;
    protocol::Header parsed;
    Bytes body;
    std::unordered_set<std::uint64_t> active;
    std::deque<Output> output;
    Clock::time_point last_activity = Clock::now(), read_start = Clock::now(), write_progress = Clock::now();
    explicit Connection(int fd) : socket(fd) {}
};
Bytes serialize(protocol::Type type, std::uint64_t id, const nlohmann::json& json) {
    const auto s = json.dump();
    if (s.size() > 65536 - protocol::header_size) throw std::length_error("response exceeds 64 KiB");
    return protocol::packet(type, id, Bytes(s.begin(), s.end()));
}
Completion error_result(const Job& job, const std::string& code, const std::string& message) {
    return {job.session, job.id, serialize(protocol::Type::error, job.id,
        {{"request_id", job.id}, {"status", "error"}, {"code", code}, {"message", message}}), true};
}
class Server {
public:
    explicit Server(const Config& config) : c_(config),
        tasks_(c_.task_queue_capacity, c_.task_queue_bytes), results_(c_.result_queue_capacity, c_.result_queue_bytes) {
        // Block signals before workers are created; signalfd handles them in the IO thread.
        sigemptyset(&signals_); sigaddset(&signals_, SIGTERM); sigaddset(&signals_, SIGINT);
        const int rc = pthread_sigmask(SIG_BLOCK, &signals_, &old_signals_);
        if (rc) throw std::runtime_error("pthread_sigmask failed");
        masked_ = true;
    }
    ~Server() {
        // Results close first: workers must never remain blocked publishing during unwinding.
        results_.close(true); tasks_.close(true);
        if (pool_) pool_->join();
        if (masked_) pthread_sigmask(SIG_SETMASK, &old_signals_, nullptr);
    }
    int run() {
        c_.validate();
        cv::setNumThreads(0); // Our worker pool owns parallelism; no nested TBB scheduler.
        epoll_.reset(epoll_create1(EPOLL_CLOEXEC)); checked(epoll_.get(), "epoll_create1");
        wake_.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)); checked(wake_.get(), "eventfd");
        signal_.reset(signalfd(-1, &signals_, SFD_NONBLOCK | SFD_CLOEXEC)); checked(signal_.get(), "signalfd");
        listener_.reset(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)); checked(listener_.get(), "socket");
        int reuse = 1; checked(setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)), "setsockopt");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<std::uint16_t>(c_.port));
        if (inet_pton(AF_INET, c_.listen_address.c_str(), &address.sin_addr) != 1) throw std::invalid_argument("listen_address must be IPv4");
        checked(bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)), "bind");
        checked(listen(listener_.get(), c_.max_clients), "listen");
        watch(listener_.get(), 1, EPOLLIN, EPOLL_CTL_ADD);
        watch(wake_.get(), 2, EPOLLIN, EPOLL_CTL_ADD);
        watch(signal_.get(), 3, EPOLLIN, EPOLL_CTL_ADD);
        for (int i = 0; i < c_.worker_threads; ++i) processors_.push_back(create_processor(c_.processor, c_.max_lines));
        pool_ = std::make_unique<ThreadPool<Job>>(tasks_, static_cast<std::size_t>(c_.worker_threads),
            [this](Job&& job, std::size_t index) { process(std::move(job), index); });
        Logger::instance().log("info", "listening " + c_.listen_address + ":" + std::to_string(c_.port));
        auto stats_at = Clock::now();
        while (true) {
            epoll_event events[64]{};
            const int n = epoll_wait(epoll_.get(), events, 64, 100);
            if (n < 0 && errno != EINTR) checked(n, "epoll_wait");
            for (int i = 0; i < n; ++i) {
                const auto token = events[i].data.u64;
                if (token == 1) { if (!stopping_) accept_clients(); }
                else if (token == 2) { std::uint64_t value; while (read(wake_.get(), &value, sizeof(value)) > 0) {} }
                else if (token == 3) {
                    signalfd_siginfo info{};
                    while (read(signal_.get(), &info, sizeof(info)) > 0) {}
                    stop();
                } else {
                    auto it = connections_.find(token);
                    if (it == connections_.end()) continue;
                    bool ok = true;
                    try {
                        if (events[i].events & EPOLLIN) ok = receive(token, it->second);
                        if (ok && (events[i].events & EPOLLOUT)) ok = send_output(it->second);
                        if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) ok = false;
                    } catch (const std::exception& e) {
                        ++protocol_errors_; Logger::instance().log("error", e.what(), token); ok = false;
                    }
                    if (!ok) disconnect(token);
                }
            }
            drain_results();
            // Offline clients can hold one complete frame until the shared queue has room.
            // Rotate the first client considered so saturation does not always favor one session.
            std::vector<std::uint64_t> ids;
            for (const auto& entry : connections_) ids.push_back(entry.first);
            if (!ids.empty()) std::rotate(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(round_++ % ids.size()), ids.end());
            for (const auto id : ids) {
                auto it = connections_.find(id);
                if (it == connections_.end()) continue;
                auto& connection = it->second;
                if (!stopping_ && !connection.close_requested && complete(connection)) submit(id, connection);
                const auto now = Clock::now();
                const bool timed_out = (connection.header_used && !complete(connection) && milliseconds(now - connection.read_start) > c_.io_timeout_ms)
                    || (!connection.output.empty() && milliseconds(now - connection.write_progress) > c_.io_timeout_ms)
                    || (connection.active.empty() && milliseconds(now - connection.last_activity) > c_.idle_timeout_ms);
                if (connection.close_requested || timed_out || (stopping_ && connection.active.empty())) disconnect(id);
                else update(id, connection);
            }
            if (milliseconds(Clock::now() - stats_at) >= 5000) { statistics(); stats_at = Clock::now(); }
            if (stopping_ && (pending_ == 0 || milliseconds(Clock::now() - stopped_at_) > c_.shutdown_timeout_ms)) {
                bool sends = false;
                for (const auto& entry : connections_) sends = sends || !entry.second.output.empty();
                if (!sends || milliseconds(Clock::now() - stopped_at_) > c_.shutdown_timeout_ms) break;
            }
            if (pool_->failures()) throw std::runtime_error("worker failed while publishing a result");
        }
        results_.close(true); tasks_.close(true); pool_->join(); connections_.clear();
        statistics();
        Logger::instance().log("info", "server stopped");
        return 0;
    }
private:
    void watch(int fd, std::uint64_t token, std::uint32_t flags, int operation) {
        epoll_event event{}; event.data.u64 = token; event.events = flags;
        checked(epoll_ctl(epoll_.get(), operation, fd, &event), "epoll_ctl");
    }
    bool complete(const Connection& connection) const {
        return connection.header_used == protocol::header_size && connection.body_used == connection.body.size();
    }
    bool readable(const Connection& connection) const {
        return !stopping_ && !connection.close_requested && !complete(connection)
            && connection.active.size() < static_cast<std::size_t>(c_.max_inflight_per_client);
    }
    void update(std::uint64_t id, Connection& connection) {
        std::uint32_t flags = EPOLLRDHUP;
        if (readable(connection)) flags |= EPOLLIN;
        if (!connection.output.empty()) flags |= EPOLLOUT;
        watch(connection.socket.get(), id, flags, EPOLL_CTL_MOD);
    }
    void accept_clients() {
        for (int count = 0; count < 32; ++count) {
            UniqueFd fd(accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
            if (fd.get() < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                Logger::instance().log("error", std::string("accept: ") + std::strerror(errno)); break;
            }
            if (connections_.size() >= static_cast<std::size_t>(c_.max_clients)) { ++rejected_; continue; }
            const int send_limit = static_cast<int>(c_.max_send_buffer_bytes);
            checked(setsockopt(fd.get(), SOL_SOCKET, SO_SNDBUF, &send_limit, sizeof(send_limit)), "SO_SNDBUF");
            const auto id = next_session_++;
            auto [it, inserted] = connections_.try_emplace(id, fd.get());
            (void)inserted;
            fd.release();
            watch(it->second.socket.get(), id, EPOLLIN | EPOLLRDHUP, EPOLL_CTL_ADD);
            ++accepted_;
        }
    }
    void disconnect(std::uint64_t id) {
        const auto it = connections_.find(id);
        if (it == connections_.end()) return;
        epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, it->second.socket.get(), nullptr);
        connections_.erase(it);
    }
    bool receive(std::uint64_t session, Connection& connection) {
        std::size_t budget = 256 * 1024; // Fairness: bound work per ready connection per iteration.
        while (budget && readable(connection)) {
            const bool header = connection.header_used < protocol::header_size;
            auto* target = header ? connection.header.data() + connection.header_used : connection.body.data() + connection.body_used;
            const auto remaining = header ? protocol::header_size - connection.header_used : connection.body.size() - connection.body_used;
            const auto n = recv(connection.socket.get(), target, std::min(remaining, budget), 0);
            if (n == 0) return false;
            if (n < 0) {
                if (errno == EINTR) continue;
                return errno == EAGAIN || errno == EWOULDBLOCK;
            }
            if (!connection.header_used) connection.read_start = Clock::now();
            connection.last_activity = Clock::now(); budget -= static_cast<std::size_t>(n);
            if (header) {
                connection.header_used += static_cast<std::size_t>(n);
                if (connection.header_used == protocol::header_size) {
                    connection.parsed = protocol::decode(connection.header.data(), protocol::header_size, c_.max_payload_bytes);
                    if (connection.parsed.type != protocol::Type::image) throw std::runtime_error("client must send image requests");
                    if (connection.active.count(connection.parsed.request_id)) throw std::runtime_error("duplicate active request_id");
                    connection.body.resize(connection.parsed.payload_length);
                }
            } else connection.body_used += static_cast<std::size_t>(n);
            if (complete(connection) && !submit(session, connection)) break;
        }
        return true;
    }
    bool submit(std::uint64_t session, Connection& connection) {
        Job job{session, connection.parsed.request_id, std::move(connection.body), Clock::now()};
        const auto size = job.image.size();
        std::vector<Job> dropped;
        const bool ok = c_.mode == "realtime" ? tasks_.push_latest(std::move(job), size, dropped) : tasks_.try_push(std::move(job), size);
        if (!ok) { connection.body = std::move(job.image); return false; }
        connection.active.insert(connection.parsed.request_id);
        connection.header_used = connection.body_used = 0;
        connection.body = Bytes{};
        ++requests_; ++pending_;
        for (const auto& old : dropped) {
            --pending_; ++dropped_;
            deliver(error_result(old, "dropped", "queued frame replaced by a newer request"));
        }
        return true;
    }
    void process(Job&& job, std::size_t index) {
        Completion completion;
        try {
            const auto start = Clock::now();
            auto image = decode_jpeg(job.image, c_);
            const auto decoded = Clock::now();
            auto result = processors_[index]->process({image, job.id});
            const auto processed = Clock::now();
            auto json = result.json(job.id);
            json["timings_ms"] = {{"queue", milliseconds(start - job.queued)}, {"decode", milliseconds(decoded - start)},
                {"process", milliseconds(processed - decoded)}, {"prepare", milliseconds(Clock::now() - processed)},
                {"total", milliseconds(Clock::now() - job.queued)}};
            completion = {job.session, job.id, serialize(protocol::Type::result, job.id, json), false};
        } catch (const std::invalid_argument& e) { completion = error_result(job, "invalid_image", e.what()); }
        catch (const std::exception& e) { completion = error_result(job, "processing_failed", e.what()); }
        catch (...) { completion = error_result(job, "processing_failed", "unknown algorithm error"); }
        const auto size = completion.packet.size();
        if (results_.push(std::move(completion), size)) {
            const std::uint64_t one = 1;
            while (write(wake_.get(), &one, sizeof(one)) < 0 && errno == EINTR) {}
        }
    }
    void drain_results() {
        while (auto result = results_.try_pop()) {
            --pending_;
            if (result->error) ++errors_; else ++completed_;
            deliver(std::move(*result));
        }
    }
    void deliver(Completion result) {
        auto it = connections_.find(result.session);
        if (it == connections_.end()) { ++stale_; return; }
        auto& connection = it->second;
        if (result.packet.size() > c_.max_send_buffer_bytes - connection.send_bytes) {
            ++slow_clients_; connection.close_requested = true; return;
        }
        if (result.error) Logger::instance().log("error", "request failed: " + std::string(
            result.packet.begin() + static_cast<std::ptrdiff_t>(protocol::header_size), result.packet.end()), result.session, result.id);
        if (connection.output.empty()) connection.write_progress = Clock::now();
        connection.send_bytes += result.packet.size();
        connection.output.push_back({result.id, std::move(result.packet), 0});
    }
    bool send_output(Connection& connection) {
        std::size_t budget = 256 * 1024;
        while (!connection.output.empty() && budget) {
            auto& out = connection.output.front();
            const auto n = send(connection.socket.get(), out.packet.data() + out.offset,
                std::min(budget, out.packet.size() - out.offset), MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                return errno == EAGAIN || errno == EWOULDBLOCK;
            }
            if (n == 0) return false;
            const auto used = static_cast<std::size_t>(n);
            budget -= used; out.offset += used;
            connection.last_activity = connection.write_progress = Clock::now();
            // Charge allocated packet bytes until its buffer is actually released.
            if (out.offset == out.packet.size()) {
                connection.send_bytes -= out.packet.size();
                connection.active.erase(out.id); connection.output.pop_front();
            }
        }
        return true;
    }
    void stop() {
        if (stopping_) return;
        stopping_ = true; stopped_at_ = Clock::now();
        epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, listener_.get(), nullptr); listener_.reset();
        tasks_.close();
        Logger::instance().log("info", "draining accepted tasks");
    }
    void statistics() {
        Logger::instance().log("info", nlohmann::json{{"requests", requests_}, {"completed", completed_}, {"errors", errors_},
            {"dropped", dropped_}, {"stale_results", stale_}, {"protocol_errors", protocol_errors_},
            {"clients", connections_.size()}, {"accepted", accepted_}, {"rejected", rejected_}, {"slow_clients", slow_clients_},
            {"pending", pending_}, {"task_depth", tasks_.size()}, {"task_peak", tasks_.peak()},
            {"result_depth", results_.size()}, {"result_peak", results_.peak()}}.dump());
    }
    Config c_;
    BoundedQueue<Job> tasks_;
    BoundedQueue<Completion> results_;
    UniqueFd epoll_, listener_, wake_, signal_;
    std::vector<std::unique_ptr<IProcessor>> processors_;
    std::unique_ptr<ThreadPool<Job>> pool_;
    std::unordered_map<std::uint64_t, Connection> connections_;
    sigset_t signals_{}, old_signals_{};
    bool masked_ = false, stopping_ = false;
    Clock::time_point stopped_at_;
    std::uint64_t next_session_ = 4, requests_ = 0, completed_ = 0, errors_ = 0, dropped_ = 0, stale_ = 0;
    std::uint64_t protocol_errors_ = 0, accepted_ = 0, rejected_ = 0, slow_clients_ = 0, pending_ = 0, round_ = 0;
};
}
int run_server(const Config& config) { Server server(config); return server.run(); }
}
