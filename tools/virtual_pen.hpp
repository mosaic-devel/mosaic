#pragma once

// A uinput-backed virtual stylus for the S19 Arc C input spikes (docs/tablet.md §4).
//
// Creates a kernel input device that libinput classifies as a tablet (BTN_TOOL_PEN +
// absolute X/Y with resolution + pressure + tilt), so the compositor exercises the SAME
// code path a physical pen would -- udev hotplug, libinput tablet interface, zwp_tablet_v2
// routing. Nothing here is Wayland-specific; the same pen drives the X11/XI2 spike.
//
// Event batching mirrors real hardware: the setters queue EV_* records and sync() closes
// the report with SYN_REPORT -- libinput coalesces per report, so one report should carry
// the full (x, y, pressure, tilt) state of that instant.
//
// Requires write access to /dev/uinput (root, uinput group, or an ACL).

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

namespace mosaic::spike {

class VirtualPen;

// ⚠⚠ A KILLED HARNESS LEAVES THE PEN DOWN, AND THAT BREAKS THE USER'S SESSION.
//
// Closing the uinput fd destroys the kernel device, but it does NOT synthesize a tip-up or a
// proximity-out on the way out. So a harness that is killed mid-stroke -- a `timeout`, a Ctrl-C, a
// crash, a failed steer that ran past its deadline -- leaves the compositor believing a tablet tool
// is still IN CONTACT. The tool holds the pointer, and **MOUSE CLICKS STOP WORKING SESSION-WIDE**
// until some real tablet event clears it. Observed live on the user's own desktop (2026-07-11): they
// could not open a file with the mouse, only with the pen, and it cleared the moment they touched the
// tablet. That was this harness, not Mosaic.
//
// So releasing the tool is not the happy path's job. Every abnormal exit does it too.
namespace detail {

inline VirtualPen* g_activePen = nullptr; // the one pen a harness may own; see VirtualPen::create

void releaseActivePen() noexcept; // defined below, once VirtualPen is complete

inline void penSignalHandler(int sig) {
    releaseActivePen();
    // write(2) is async-signal-safe; printf is not. This line is the harness saying, out loud, that
    // it did not leave a tool in contact on the user's desktop.
    static constexpr char kMsg[] = "virtual pen: released the tool on signal\n";
    (void)!::write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
    // Re-raise with the default disposition so the exit status still says what killed us.
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

inline void installPenSignalHandlers() {
    for (const int sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGABRT})
        std::signal(sig, penSignalHandler);
}

} // namespace detail

class VirtualPen {
public:
    // Axis ranges: a medium ~224x140 mm tablet at 200 units/mm. Pressure and tilt ranges
    // copy the common Wacom shape (16-bit pressure, +-64 degree tilt, resolution 0 so
    // libinput derives degrees from the range).
    static constexpr std::int32_t kMaxX = 44704;
    static constexpr std::int32_t kMaxY = 27940;
    static constexpr std::int32_t kMaxPressure = 65535;
    static constexpr std::int32_t kTiltRange = 64;

    VirtualPen() = default;
    ~VirtualPen() { destroy(); }
    VirtualPen(const VirtualPen&) = delete;
    VirtualPen& operator=(const VirtualPen&) = delete;

    bool create(std::string& error) {
        m_fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (m_fd < 0) {
            error = std::string("open /dev/uinput: ") + std::strerror(errno);
            return false;
        }
        bool ok = ioctl(m_fd, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(m_fd, UI_SET_EVBIT, EV_ABS) == 0
            && ioctl(m_fd, UI_SET_KEYBIT, BTN_TOOL_PEN) == 0
            && ioctl(m_fd, UI_SET_KEYBIT, BTN_TOUCH) == 0
            && ioctl(m_fd, UI_SET_KEYBIT, BTN_STYLUS) == 0;
        ok = ok && absAxis(ABS_X, 0, kMaxX, 200) && absAxis(ABS_Y, 0, kMaxY, 200)
            && absAxis(ABS_PRESSURE, 0, kMaxPressure, 0)
            && absAxis(ABS_TILT_X, -kTiltRange, kTiltRange, 0)
            && absAxis(ABS_TILT_Y, -kTiltRange, kTiltRange, 0);
        if (ok) {
            uinput_setup setup{};
            setup.id.bustype = BUS_USB;
            setup.id.vendor = 0x6d73;  // "ms" -- deliberately NOT a real vendor id
            setup.id.product = 0x0001;
            std::strncpy(setup.name, "Mosaic Spike Pen", sizeof(setup.name) - 1);
            ok = ioctl(m_fd, UI_DEV_SETUP, &setup) == 0 && ioctl(m_fd, UI_DEV_CREATE) == 0;
        }
        if (!ok) {
            error = std::string("uinput device setup: ") + std::strerror(errno);
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        // From here on this pen is releasable from a signal handler: a killed harness must not leave
        // a tool in contact on the user's desktop (see the note above this class).
        detail::g_activePen = this;
        detail::installPenSignalHandlers();
        return true;
    }

    // Lift the tip and the tool, unconditionally. Async-signal-safe: write() and ioctl() are, and it
    // touches nothing else. Called by destroy() AND by the signal handler, so it must be safe twice.
    void release() noexcept {
        if (m_fd < 0)
            return;
        // NOT gated on m_inProximity. That flag tracks what we THINK we sent, and the whole point
        // here is the paths where we cannot be sure -- a crash between proximityIn() and its sync(),
        // a kill mid-stroke. Lifting a tool that was never down is harmless; leaving one down is not.
        emit(EV_KEY, BTN_TOUCH, 0);
        emit(EV_ABS, ABS_PRESSURE, 0);
        emit(EV_KEY, BTN_STYLUS, 0);
        emit(EV_KEY, BTN_TOOL_PEN, 0);
        emit(EV_SYN, SYN_REPORT, 0);
        m_inProximity = false;
    }

    void destroy() {
        if (m_fd < 0)
            return;
        release(); // leave no phantom tool behind
        ioctl(m_fd, UI_DEV_DESTROY);
        ::close(m_fd);
        m_fd = -1;
        if (detail::g_activePen == this)
            detail::g_activePen = nullptr;
    }

    [[nodiscard]] bool valid() const { return m_fd >= 0; }
    [[nodiscard]] std::int32_t x() const { return m_x; }
    [[nodiscard]] std::int32_t y() const { return m_y; }

    // Report builders -- each queues events; close the report with sync().
    void proximityIn(std::int32_t x, std::int32_t y) {
        emit(EV_KEY, BTN_TOOL_PEN, 1);
        move(x, y);
        emit(EV_ABS, ABS_PRESSURE, 0);
        m_inProximity = true;
    }
    void proximityOut() {
        emit(EV_KEY, BTN_TOOL_PEN, 0);
        m_inProximity = false;
    }
    void move(std::int32_t x, std::int32_t y) {
        m_x = clamp(x, 0, kMaxX);
        m_y = clamp(y, 0, kMaxY);
        emit(EV_ABS, ABS_X, m_x);
        emit(EV_ABS, ABS_Y, m_y);
    }
    void pressure(std::int32_t p) { emit(EV_ABS, ABS_PRESSURE, clamp(p, 0, kMaxPressure)); }
    void tilt(std::int32_t tx, std::int32_t ty) {
        emit(EV_ABS, ABS_TILT_X, clamp(tx, -kTiltRange, kTiltRange));
        emit(EV_ABS, ABS_TILT_Y, clamp(ty, -kTiltRange, kTiltRange));
    }
    void tipDown() { emit(EV_KEY, BTN_TOUCH, 1); }
    void tipUp() { emit(EV_KEY, BTN_TOUCH, 0); }
    void sync() { emit(EV_SYN, SYN_REPORT, 0); }

private:
    static std::int32_t clamp(std::int32_t v, std::int32_t lo, std::int32_t hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    bool absAxis(std::uint16_t code, std::int32_t lo, std::int32_t hi, std::int32_t res) {
        uinput_abs_setup abs{};
        abs.code = code;
        abs.absinfo.minimum = lo;
        abs.absinfo.maximum = hi;
        abs.absinfo.resolution = res;
        return ioctl(m_fd, UI_SET_ABSBIT, code) == 0 && ioctl(m_fd, UI_ABS_SETUP, &abs) == 0;
    }

    void emit(std::uint16_t type, std::uint16_t code, std::int32_t value) {
        if (m_fd < 0)
            return;
        input_event ev{};
        ev.type = type;
        ev.code = code;
        ev.value = value;
        // Best effort: a full uinput queue drops the report; the spike's dwell/retry logic
        // tolerates lost motion.
        (void)!::write(m_fd, &ev, sizeof(ev));
    }

    int m_fd = -1;
    bool m_inProximity = false;
    std::int32_t m_x = 0;
    std::int32_t m_y = 0;
};

namespace detail {
inline void releaseActivePen() noexcept {
    if (g_activePen != nullptr)
        g_activePen->release();
}
} // namespace detail

} // namespace mosaic::spike
