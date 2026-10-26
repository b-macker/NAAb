// A governed program run in a fresh `naab-lang run` worker process (see
// worker.h). Moved here from mcp_server.cpp when REST /execute began to use
// it; the behaviour is unchanged.
#ifndef _WIN32

#include "worker.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <random>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace naab {
namespace api {

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

void setNonBlockingCloexec(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int fd_fl = fcntl(fd, F_GETFD);
    if (fd_fl >= 0) fcntl(fd, F_SETFD, fd_fl | FD_CLOEXEC);
}

bool makePipe(int p[2]) {
    if (pipe(p) != 0) return false;
    setNonBlockingCloexec(p[0]);
    int fl = fcntl(p[1], F_GETFD);
    if (fl >= 0) fcntl(p[1], F_SETFD, fl | FD_CLOEXEC);
    return true;
}

}  // namespace

std::string randomHex(size_t bytes) {
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        unsigned v = rd() & 0xffu;
        out.push_back(hex[v >> 4]);
        out.push_back(hex[v & 0xf]);
    }
    return out;
}

std::string writeWorkerProgram(const std::string& dir, const std::string& prefix,
                               const std::string& code) {
    std::string path = dir + "/" + prefix + randomHex(8) + ".naab";
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return "";
    const char* p = code.data();
    size_t left = code.size();
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(path.c_str());
            return "";
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    close(fd);
    return path;
}

void removeStaleWorkerPrograms(const std::string& dir, const std::string& prefix) {
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string name = e.path().filename().string();
        if (name.rfind(prefix, 0) != 0 || e.path().extension() != ".naab") continue;
        auto t = fs::last_write_time(e.path(), ec);
        if (ec) continue;
        if (now - t > std::chrono::hours(24)) fs::remove(e.path(), ec);
    }
}

// After the worker exits, anything left in its process group is killed: a call
// must not leave a background process behind.
WorkerResult runWorker(const std::string& self, const std::string& cwd,
                       const std::vector<std::string>& args, int timeout_seconds,
                       const std::shared_ptr<InFlight>& fl,
                       const std::function<bool(const std::string&)>& on_event) {
    WorkerResult r;
    int outp[2], errp[2], evp[2];
    if (!makePipe(outp)) return r;
    if (!makePipe(errp)) { close(outp[0]); close(outp[1]); return r; }
    if (!makePipe(evp)) {
        close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
        return r;
    }

    // Everything the child needs is built before fork(): after it, only
    // async-signal-safe calls are allowed in a multithreaded process.
    std::vector<std::string> argv_s;
    argv_s.push_back(self);
    argv_s.push_back("run");
    for (const auto& a : args) argv_s.push_back(a);
    std::vector<char*> argv;
    for (auto& a : argv_s) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const char* self_c = self.c_str();
    const char* cwd_c = cwd.c_str();
    // The server ignores SIGPIPE; an ignored disposition survives exec, so the
    // worker gets the default back (a worker whose server is gone should stop).
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);

    pid_t pid = fork();
    if (pid < 0) {
        close(outp[0]); close(outp[1]); close(errp[0]); close(errp[1]);
        close(evp[0]); close(evp[1]);
        return r;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (chdir(cwd_c) != 0) _exit(126);
        int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devnull >= 0) dup2(devnull, 0);
        dup2(outp[1], 1);
        dup2(errp[1], 2);
        // The event stream is always descriptor 3 in the worker. dup2 onto
        // itself would keep close-on-exec, so clear it explicitly.
        if (evp[1] == 3) {
            int f = fcntl(3, F_GETFD);
            if (f >= 0) fcntl(3, F_SETFD, f & ~FD_CLOEXEC);
        } else {
            dup2(evp[1], 3);
        }
        sigaction(SIGPIPE, &dfl, nullptr);
        execv(self_c, argv.data());
        _exit(127);
    }
    setpgid(pid, pid);  // also in the child; whichever runs first wins the race
    close(outp[1]); close(errp[1]); close(evp[1]);
    r.started = true;
    {
        std::lock_guard<std::mutex> lk(fl->mu);
        fl->pid = pid;
        if (fl->cancelled) kill(-pid, SIGKILL);
    }

    const auto deadline = Clock::now() + std::chrono::seconds(timeout_seconds) +
                          std::chrono::seconds(5);
    std::string ev_buf;
    bool out_open = true, err_open = true, ev_open = true;
    bool exited = false;
    int status = 0;
    Clock::time_point exited_at{};
    char buf[65536];

    auto drainEvents = [&]() {
        size_t pos;
        while ((pos = ev_buf.find('\n')) != std::string::npos) {
            std::string line = ev_buf.substr(0, pos);
            ev_buf.erase(0, pos + 1);
            if (line.empty() || line.size() > kWorkerMaxEventLine) continue;
            if (on_event(line)) r.events++;
        }
        // A line longer than any real event is not one: drop it rather than
        // buffering without bound.
        if (ev_buf.size() > kWorkerMaxEventLine) ev_buf.clear();
    };

    while (out_open || err_open || ev_open || !exited) {
        struct pollfd pfds[3];
        int n = 0;
        int idx_out = -1, idx_err = -1, idx_ev = -1;
        if (out_open) { pfds[n] = {outp[0], POLLIN, 0}; idx_out = n++; }
        if (err_open) { pfds[n] = {errp[0], POLLIN, 0}; idx_err = n++; }
        if (ev_open)  { pfds[n] = {evp[0], POLLIN, 0};  idx_ev = n++; }
        if (n > 0) poll(pfds, static_cast<nfds_t>(n), 50);
        else std::this_thread::sleep_for(std::chrono::milliseconds(20));

        auto readInto = [&](int fd, bool& open_flag, auto&& sink) {
            for (;;) {
                ssize_t k = read(fd, buf, sizeof(buf));
                if (k > 0) { sink(buf, static_cast<size_t>(k)); continue; }
                if (k == 0) { open_flag = false; return; }
                if (errno == EINTR) continue;
                return;  // EAGAIN
            }
        };
        if (idx_out >= 0 && (pfds[idx_out].revents & (POLLIN | POLLHUP | POLLERR)))
            readInto(outp[0], out_open, [&](const char* p, size_t k) {
                if (r.out.size() < kWorkerMaxOutput)
                    r.out.append(p, std::min(k, kWorkerMaxOutput - r.out.size()));
                if (r.out.size() >= kWorkerMaxOutput) r.out_truncated = true;
            });
        if (idx_err >= 0 && (pfds[idx_err].revents & (POLLIN | POLLHUP | POLLERR)))
            readInto(errp[0], err_open, [&](const char* p, size_t k) {
                if (r.err.size() < kWorkerMaxDiagnostics)
                    r.err.append(p, std::min(k, kWorkerMaxDiagnostics - r.err.size()));
            });
        if (idx_ev >= 0 && (pfds[idx_ev].revents & (POLLIN | POLLHUP | POLLERR))) {
            readInto(evp[0], ev_open, [&](const char* p, size_t k) { ev_buf.append(p, k); });
            drainEvents();
        }

        if (!exited) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                exited = true;
                exited_at = Clock::now();
                // Whatever the worker left in its group goes with it.
                kill(-pid, SIGKILL);
            }
        }
        bool cancelled_now;
        {
            std::lock_guard<std::mutex> lk(fl->mu);
            cancelled_now = fl->cancelled;
        }
        if (!exited && cancelled_now && !r.cancelled) {
            r.cancelled = true;
            kill(-pid, SIGKILL);
        }
        if (!exited && !r.timed_out && Clock::now() > deadline) {
            r.timed_out = true;
            kill(-pid, SIGKILL);
        }
        // A grandchild that escaped the group can hold a pipe open after the
        // worker is gone; stop reading shortly after the exit.
        if (exited && Clock::now() - exited_at > std::chrono::milliseconds(300)) break;
    }
    if (!exited) waitpid(pid, &status, 0);
    drainEvents();
    close(outp[0]); close(errp[0]); close(evp[0]);
    {
        std::lock_guard<std::mutex> lk(fl->mu);
        fl->pid = -1;
    }
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.term_signal = WTERMSIG(status);
    return r;
}

}  // namespace api
}  // namespace naab

#endif  // _WIN32
