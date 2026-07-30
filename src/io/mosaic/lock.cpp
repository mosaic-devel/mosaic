#include "io/mosaic/lock.hpp"

#include "io/mosaic/journal.hpp" // recoveryJournalPath -- the lock shares the journal's key

#include "common/fs_path.hpp" // pathFromUtf8: the recovery dir carries the user's account name

#include <filesystem>
#include <utility>

#ifdef _WIN32
// See save.cpp: the toolchain defines NOMINMAX, and redefining it is a -Werror diagnostic.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace mosaic::io::native {

std::string recoveryLockPath(const std::string& uuid, const std::string& canonicalDocumentPath) {
    return recoveryJournalPath(uuid, canonicalDocumentPath) + ".lock";
}

namespace {
void ensureParentDir(const std::string& path) {
    // pathFromUtf8, not the std::string constructor: on Windows a std::filesystem::path is
    // wchar_t-based and a narrow string is decoded in the ACTIVE CODE PAGE, so the UTF-8 bytes of
    // "C:\Users\Zoë\AppData\Local\mosaic\recovery" would name a directory nobody asked for.
    const std::filesystem::path p = common::pathFromUtf8(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec); // best-effort; open reports truth
    }
}
} // namespace

AdvisoryLock::AdvisoryLock(AdvisoryLock&& other) noexcept
    :
#ifdef _WIN32
      handle_(std::exchange(other.handle_, nullptr)),
#else
      fd_(std::exchange(other.fd_, -1)),
#endif
      path_(std::move(other.path_)) {
}

AdvisoryLock& AdvisoryLock::operator=(AdvisoryLock&& other) noexcept {
    if (this != &other) {
        release();
#ifdef _WIN32
        handle_ = std::exchange(other.handle_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
        path_ = std::move(other.path_);
    }
    return *this;
}

AdvisoryLock::~AdvisoryLock() { release(); }

#ifdef _WIN32

AdvisoryLock::Status AdvisoryLock::tryAcquire(const std::string& path,
                                             std::optional<AdvisoryLock>& out, std::string* error) {
    ensureParentDir(path);
    // Exclusive open (dwShareMode 0): while this instance holds the handle, a second instance's
    // open fails with ERROR_SHARING_VIOLATION -- that IS the Busy verdict, with no lock byte, no
    // heartbeat and no PID check anywhere in it.
    //
    // AUTO-RELEASE ON A DEAD HOLDER, which is the §2.10 property the whole design leans on: a
    // Win32 handle is owned by the process, and the kernel closes every handle a process held when
    // it dies -- kill, crash, or power-off-then-reboot alike. So the sharing violation disappears
    // the instant the holder stops existing, and a lock FILE left behind by a crash reopens
    // cleanly on the very next try. That is the same guarantee flock() gives on POSIX, arrived at
    // the same way (an OS-owned handle, not a recorded PID), so the stale-lock case folds into
    // ordinary journal-backed crash recovery on both platforms with no liveness ping and no
    // PID-reuse hazard. It also means the lock cannot outlive its holder, which is exactly why
    // §2.10 can call it prevention and still put the load-bearing weight on the tail check.
    const HANDLE h = ::CreateFileW(common::pathFromUtf8(path).c_str(),
                                   GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
            return Status::Busy;
        if (error != nullptr)
            *error = "could not open the recovery lock file";
        return Status::Error;
    }
    // Record our pid, purely informational (a human inspecting the recovery dir), and truncate
    // first because OPEN_ALWAYS keeps a previous holder's bytes -- SetEndOfFile cuts at the current
    // file pointer, which a fresh OPEN_ALWAYS leaves at 0, so this is the ftruncate(fd, 0) below.
    // The exclusive handle, not this content, is the authority: as on POSIX a truncate or write
    // failure here never fails the acquire.
    if (::SetEndOfFile(h) != 0) {
        const unsigned long self = ::GetCurrentProcessId();
        const std::string pid = std::to_string(self) + "\n";
        DWORD wrote = 0;
        [[maybe_unused]] const BOOL ok =
            ::WriteFile(h, pid.data(), static_cast<DWORD>(pid.size()), &wrote, nullptr);
    }
    AdvisoryLock lock;
    lock.handle_ = h;
    lock.path_ = path;
    out = std::move(lock);
    return Status::Acquired;
}

void AdvisoryLock::release() {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

#else

AdvisoryLock::Status AdvisoryLock::tryAcquire(const std::string& path,
                                             std::optional<AdvisoryLock>& out, std::string* error) {
    ensureParentDir(path);
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (error != nullptr)
            *error = "could not open the recovery lock file";
        return Status::Error;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        ::close(fd);
        if (e == EWOULDBLOCK)
            return Status::Busy; // a live holder has it
        if (error != nullptr)
            *error = "could not lock the recovery lock file";
        return Status::Error;
    }
    // Record our pid, purely informational (a human inspecting the recovery dir). The flock, not
    // this content, is the authority -- so a truncate/write failure never fails the acquire.
    if (::ftruncate(fd, 0) == 0) {
        const std::string pid = std::to_string(static_cast<long>(::getpid())) + "\n";
        [[maybe_unused]] const ssize_t n = ::write(fd, pid.data(), pid.size());
    }
    AdvisoryLock lock;
    lock.fd_ = fd;
    lock.path_ = path;
    out = std::move(lock);
    return Status::Acquired;
}

void AdvisoryLock::release() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN); // explicit; close would release it anyway
        ::close(fd_);
        fd_ = -1;
    }
}

#endif

} // namespace mosaic::io::native
