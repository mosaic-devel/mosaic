#pragma once

#include <optional>
#include <string>

// mosaic/lock -- the §2.10 advisory lock: a lock file in the APP-OWNED recovery directory, NEVER a
// lock or handle on the user's document (the section-0 hard rule). A second Mosaic opening the same
// document finds this lock busy and offers a read-only open; the first instance keeps sole write
// authority, so the two-writers race the format is otherwise only RESILIENT to never even starts.
//
// It is an OS advisory lock (flock on POSIX, an exclusive handle on Windows), chosen deliberately:
// a HOLDER THAT DIES releases it automatically. So a crashed instance leaves a stale lock file the
// next open acquires cleanly -- there is no liveness ping, no PID-reuse hazard, and the stale case
// folds straight into ordinary journal-backed crash recovery (flow 1).
namespace mosaic::io::native {

// The lock path for a document: its recovery journal path + ".lock" (same uuid+pathhash key, so
// the lock and the journal name the same document). Untitled documents are not locked -- two
// untitled windows share no file to conflict over.
[[nodiscard]] std::string recoveryLockPath(const std::string& uuid,
                                           const std::string& canonicalDocumentPath);

class AdvisoryLock {
public:
    enum class Status {
        Acquired, // we now hold the lock for the session
        Busy,     // a LIVE holder has it -- offer read-only (flow 6)
        Error,    // could not even attempt (permission, path) -- proceed without a lock
    };

    // Try to take the lock at `path` (created if absent). On Acquired, `out` holds it until it is
    // released or destroyed. Busy/Error leave `out` empty.
    [[nodiscard]] static Status tryAcquire(const std::string& path,
                                           std::optional<AdvisoryLock>& out,
                                           std::string* error = nullptr);

    AdvisoryLock(AdvisoryLock&& other) noexcept;
    AdvisoryLock& operator=(AdvisoryLock&& other) noexcept;
    AdvisoryLock(const AdvisoryLock&) = delete;
    AdvisoryLock& operator=(const AdvisoryLock&) = delete;
    ~AdvisoryLock();

    // Drop the lock (the OS releases it when the handle closes). The lock FILE is left in place --
    // removing it would race a concurrent acquire on the same inode; a lingering empty lock file is
    // harmless, and the next open re-locks it cleanly.
    void release();

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    AdvisoryLock() = default;
#ifdef _WIN32
    void* handle_ = nullptr; // HANDLE; nullptr = not held
#else
    int fd_ = -1; // < 0 = not held
#endif
    std::string path_;
};

} // namespace mosaic::io::native
