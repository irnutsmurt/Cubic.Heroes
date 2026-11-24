// The launcher's side of the pipeline worker (docs/M9_WORKER_PROTOCOL.md): one operation per worker process, started
// with no console window, stdout and stdin as pipes and --control; its JSON events read on a thread of their own and
// taken by the interface each frame; "cancel" written to its stdin. Closing the launcher closes the pipe: the worker
// then cancels by itself.
#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace launcher {

#ifdef _WIN32
inline std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

inline std::string narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// one argument quoted as CreateProcess's command line parsing expects
inline std::wstring quote_arg(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { bs++; continue; }
        if (c == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; bs = 0; continue; }
        o.append(bs, L'\\');
        bs = 0;
        o += c;
    }
    o.append(bs * 2, L'\\');
    return o + L"\"";
}
#endif

class Job {
public:
    std::string name;   // the worker command
    Job() = default;
    Job(const Job&) = delete;
    ~Job() { detach(); }

    // argv[0] the program; the worker's command and options after
    bool start(const std::vector<std::string>& argv, const std::string& command)
    {
        name = command;
        done_ = false;
        exit_code_ = -1;
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
        HANDLE out_r, out_w, in_r, in_w;
        if (!CreatePipe(&out_r, &out_w, &sa, 0)) return false;
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) { CloseHandle(out_r); CloseHandle(out_w); return false; }
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si = { sizeof si };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out_w;
        si.hStdInput = in_r;
        si.hStdError = nul;
        std::wstring cmd;
        for (const std::string& a : argv) cmd += (cmd.empty() ? L"" : L" ") + quote_arg(widen(a));
        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(out_w);
        CloseHandle(in_r);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) { CloseHandle(out_r); CloseHandle(in_w); return false; }
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        in_ = in_w;
        reader_ = std::thread([this, out_r] {
            std::string line;
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(out_r, buf, sizeof buf, &n, nullptr) && n) {
                for (DWORD i = 0; i < n; i++) {
                    if (buf[i] == '\n') { take(line); line.clear(); }
                    else if (buf[i] != '\r') line += buf[i];
                }
            }
            if (!line.empty()) take(line);
            CloseHandle(out_r);
            WaitForSingleObject(proc_, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(proc_, &code);
            exit_code_ = (int)code;
            done_ = true;
        });
        return true;
#else
        int out[2], in[2];
        if (pipe(out) || pipe(in)) return false;
        pid_t pid = fork();
        if (pid == 0) {
            dup2(out[1], 1);
            dup2(in[0], 0);
            close(out[0]); close(in[1]);
            std::vector<char*> av;
            for (const std::string& a : argv) av.push_back(const_cast<char*>(a.c_str()));
            av.push_back(nullptr);
            setsid();
            execvp(av[0], av.data());
            _exit(127);
        }
        close(out[1]);
        close(in[0]);
        if (pid < 0) return false;
        pid_ = pid;
        infd_ = in[1];
        reader_ = std::thread([this, fd = out[0]] {
            std::string line;
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof buf)) > 0)
                for (ssize_t i = 0; i < n; i++) {
                    if (buf[i] == '\n') { take(line); line.clear(); }
                    else line += buf[i];
                }
            if (!line.empty()) take(line);
            close(fd);
            int st = 0;
            waitpid(pid_, &st, 0);
            exit_code_ = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
            done_ = true;
        });
        return true;
#endif
    }

    // the events since the last call
    std::vector<json::Value> take_events()
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<json::Value> out(events_.begin(), events_.end());
        events_.clear();
        return out;
    }

    void cancel()
    {
#ifdef _WIN32
        if (in_) {
            DWORD n;
            WriteFile(in_, "cancel\n", 7, &n, nullptr);
        }
#else
        if (infd_ >= 0) { ssize_t r = write(infd_, "cancel\n", 7); (void)r; }
#endif
        cancel_sent = true;
    }

    // M12: one line to the worker's control channel (the PS3 save key the player typed: never on its command line)
    void send_line(const std::string& line)
    {
        const std::string l = line + "\n";
#ifdef _WIN32
        if (in_) {
            DWORD n;
            WriteFile(in_, l.data(), (DWORD)l.size(), &n, nullptr);
        }
#else
        if (infd_ >= 0) { ssize_t r = write(infd_, l.data(), l.size()); (void)r; }
#endif
    }

    bool done() const { return done_; }
    int exit_code() const { return exit_code_; }
    const json::Value& result() const { return result_; }
    bool cancel_sent = false;

private:
    std::mutex mu_;
    std::deque<json::Value> events_;
    json::Value result_;
    std::thread reader_;
    std::atomic<bool> done_{ true };
    std::atomic<int> exit_code_{ -1 };
#ifdef _WIN32
    HANDLE proc_ = nullptr, in_ = nullptr;
#else
    pid_t pid_ = -1;
    int infd_ = -1;
#endif

    void take(const std::string& line)
    {
        json::Value v;
        if (!json::parse(line, v)) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (v["ev"].str() == "result") result_ = v;
        events_.push_back(std::move(v));
    }

    void detach()
    {
#ifdef _WIN32
        if (in_) { CloseHandle(in_); in_ = nullptr; }   // the worker cancels when its stdin ends
#else
        if (infd_ >= 0) { close(infd_); infd_ = -1; }
#endif
        if (reader_.joinable()) reader_.join();
#ifdef _WIN32
        if (proc_) { CloseHandle(proc_); proc_ = nullptr; }
#endif
    }
};

}  // namespace launcher
