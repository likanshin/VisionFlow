#include "visionflow/core/fd.hpp"
#include <spawn.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <poll.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <vector>
extern char** environ;
int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        std::cout << "vision_supervisor SERVER [SERVER_ARGS...]\nForwards SIGINT/SIGTERM; reaps child; kills after 10s grace.\n";
        return argc < 2 ? 1 : 0;
    }
    sigset_t mask, old;
    sigemptyset(&mask); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGINT); sigaddset(&mask, SIGCHLD);
    if (pthread_sigmask(SIG_BLOCK, &mask, &old)) return 1;
    visionflow::UniqueFd signals(signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK));
    if (signals.get() < 0) { std::cerr << "signalfd failed\n"; return 1; }
    posix_spawnattr_t attributes;
    int rc = posix_spawnattr_init(&attributes);
    if (rc) { std::cerr << "spawn attributes: " << std::strerror(rc) << '\n'; return 1; }
    sigset_t empty; sigemptyset(&empty);
    rc = posix_spawnattr_setsigmask(&attributes, &empty);
    if (!rc) rc = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK);
    pid_t child = -1;
    if (!rc) rc = posix_spawnp(&child, argv[1], nullptr, &attributes, argv + 1, environ);
    posix_spawnattr_destroy(&attributes);
    if (rc) { std::cerr << "spawn: " << std::strerror(rc) << '\n'; return 1; }
    std::cerr << "supervisor child_pid=" << child << '\n';
    bool stopping = false;
    auto deadline = std::chrono::steady_clock::now();
    for (;;) {
        int status = 0;
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            std::cerr << "supervisor child reaped status=" << status << '\n';
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        if (waited < 0 && errno != EINTR) return 1;
        pollfd pfd{signals.get(), POLLIN, 0};
        if (poll(&pfd, 1, 100) < 0 && errno != EINTR) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} return 1; }
        signalfd_siginfo info{};
        while (read(signals.get(), &info, sizeof(info)) == sizeof(info)) {
            if ((info.ssi_signo == SIGTERM || info.ssi_signo == SIGINT) && !stopping) {
                kill(child, SIGTERM); stopping = true;
                deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            }
        }
        if (stopping && std::chrono::steady_clock::now() >= deadline) kill(child, SIGKILL);
    }
}
