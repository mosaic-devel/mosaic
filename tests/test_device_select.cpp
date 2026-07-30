#include "render/gpu_caps.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

// The `--device` picker (S60-f, docs/s60-performance-plan.md section 9).
//
// Every case here drives `pickPhysicalDevice` against a device list that does not exist. That is
// the whole reason the selection policy was extracted as a free function over plain data: the
// interesting configurations are the ones nobody's dev box has -- a hybrid laptop, a machine whose
// only device is a software rasterizer, two cards whose names share a substring -- and none of
// them can be tested by owning hardware.

using namespace mosaic::render;

namespace {

DeviceInfo dev(std::uint32_t index, std::string name, VkPhysicalDeviceType type) {
    DeviceInfo d;
    d.index = index;
    d.name = std::move(name);
    d.type = type;
    return d;
}

DeviceSelector flagSelector(std::string text) {
    return DeviceSelector{std::move(text), /*fromEnvironment=*/false};
}

// The hybrid laptop this whole slice exists for: a discrete part and the integrated one, in the
// order a driver is entirely free to enumerate them.
std::vector<DeviceInfo> hybridLaptop() {
    return {dev(0, "Intel(R) UHD Graphics 620", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU),
            dev(1, "NVIDIA GeForce GTX 1650", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)};
}

}  // namespace

TEST_CASE("the automatic pick ranks discrete > integrated > virtual > cpu") {
    // Deliberately enumerated worst-first, so a picker that merely takes the first usable device
    // would pass nothing here.
    const std::vector<DeviceInfo> devices = {
        dev(0, "llvmpipe", VK_PHYSICAL_DEVICE_TYPE_CPU),
        dev(1, "Something Unlabelled", VK_PHYSICAL_DEVICE_TYPE_OTHER),
        dev(2, "Virtio GPU", VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU),
        dev(3, "Intel Iris", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU),
        dev(4, "Radeon RX 7900", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU),
    };

    CHECK(pickPhysicalDevice(devices, DeviceSelector{}).index == 4);

    // And the ordering itself, so a future edit to the rank table has to say so out loud.
    CHECK(deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) <
          deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU));
    CHECK(deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) <
          deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU));
    CHECK(deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) <
          deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_CPU));
    // OTHER is unknown, which is not the same as known-to-be-software: it outranks cpu and
    // nothing else.
    CHECK(deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) <
          deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_OTHER));
    CHECK(deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_OTHER) <
          deviceTypeRank(VK_PHYSICAL_DEVICE_TYPE_CPU));
}

TEST_CASE("the automatic pick breaks a rank tie on the lowest enumeration index") {
    const std::vector<DeviceInfo> devices = {
        dev(0, "Radeon A", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU),
        dev(1, "Radeon B", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU),
    };
    CHECK(pickPhysicalDevice(devices, DeviceSelector{}).index == 0);
}

TEST_CASE("an unusable device is never picked, however well it ranks") {
    std::vector<DeviceInfo> devices = hybridLaptop();
    devices[1].usable = false;  // the discrete part cannot present / has no graphics queue
    devices[1].unusableReason = "no graphics queue family";

    const DeviceChoice choice = pickPhysicalDevice(devices, DeviceSelector{});
    CHECK(choice.index == 0);
    CHECK(choice.warning.empty());  // nothing the user asked for was ignored
}

TEST_CASE("--device selects by index") {
    const std::vector<DeviceInfo> devices = hybridLaptop();

    const DeviceChoice integrated = pickPhysicalDevice(devices, flagSelector("0"));
    CHECK(integrated.index == 0);
    CHECK(integrated.warning.empty());
    CHECK(integrated.reason.find("index 0") != std::string::npos);

    // The point of the flag: the automatic pick would have taken [1], and here it does not have to
    // be argued with -- but it also must be possible to ASK for [1] explicitly.
    CHECK(pickPhysicalDevice(devices, flagSelector("1")).index == 1);
}

TEST_CASE("--device selects by case-insensitive name substring") {
    const std::vector<DeviceInfo> devices = hybridLaptop();

    for (const char* text : {"nvidia", "NVIDIA", "GeForce", "gEfOrCe", "GTX 16"}) {
        const DeviceChoice choice = pickPhysicalDevice(devices, flagSelector(text));
        CHECK(choice.index == 1);
        CHECK(choice.warning.empty());
    }
    // And the integrated part is reachable the same way, which is the actual hybrid-laptop fix:
    // the automatic pick takes the discrete card, and a user on battery wants the other one.
    CHECK(pickPhysicalDevice(devices, flagSelector("intel")).index == 0);
}

TEST_CASE("an all-digit selector is an index, never a name fragment") {
    // "1650" is a substring of the second device's name, and it is STILL an index -- the rule is
    // the shape of the text, not what happens to be on the machine, so `--device 1650` means the
    // same thing everywhere. It matches nothing here, which falls back with a warning.
    const std::vector<DeviceInfo> devices = hybridLaptop();
    const DeviceChoice choice = pickPhysicalDevice(devices, flagSelector("1650"));
    CHECK(choice.index == 1);  // the automatic pick, not the name match
    CHECK(choice.warning.find("index 1650") != std::string::npos);
}

TEST_CASE("a substring matching two devices takes the lowest enumeration index") {
    // The pinned tie-break. Note the second device also outranks the first -- the rule is the
    // INDEX, deliberately, so that what the user reads in the enumeration log is what they get.
    const std::vector<DeviceInfo> devices = {
        dev(0, "Radeon RX 6600", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU),
        dev(1, "Radeon RX 7900", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU),
    };

    const DeviceChoice choice = pickPhysicalDevice(devices, flagSelector("radeon"));
    CHECK(choice.index == 0);
    CHECK(choice.warning.empty());
    // The ambiguity is reported rather than hidden: two matched, one was taken, and the log says
    // which rule did it.
    CHECK(choice.reason.find("2 devices matched") != std::string::npos);
}

TEST_CASE("an exact name beats a substring of a longer name") {
    // "Intel(R) UHD Graphics" is a whole device name AND a substring of the other one. Without the
    // exact-match pass the shorter device would be unnameable.
    const std::vector<DeviceInfo> devices = {
        dev(0, "Intel(R) UHD Graphics 620", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU),
        dev(1, "Intel(R) UHD Graphics", VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU),
    };

    CHECK(pickPhysicalDevice(devices, flagSelector("Intel(R) UHD Graphics")).index == 1);
    CHECK(pickPhysicalDevice(devices, flagSelector("intel(r) uhd graphics")).index == 1);
    // No exact match => the substring pass, and then the pinned lowest-index rule.
    CHECK(pickPhysicalDevice(devices, flagSelector("uhd")).index == 0);
}

TEST_CASE("an unmatched selector falls back to the automatic pick, and says so") {
    const std::vector<DeviceInfo> devices = hybridLaptop();

    const DeviceChoice byName = pickPhysicalDevice(devices, flagSelector("matrox"));
    CHECK(byName.index == 1);  // exactly what the automatic pick would have returned
    CHECK_FALSE(byName.warning.empty());
    CHECK(byName.warning.find("matrox") != std::string::npos);
    CHECK(byName.warning.find("--device") != std::string::npos);

    // An out-of-range index is the same class of mistake and gets the same treatment -- never a
    // hard failure, because a machine that lost a GPU must still start.
    const DeviceChoice byIndex = pickPhysicalDevice(devices, flagSelector("7"));
    CHECK(byIndex.index == 1);
    CHECK_FALSE(byIndex.warning.empty());

    // The environment's mistakes are named after the environment.
    const DeviceChoice fromEnv =
        pickPhysicalDevice(devices, DeviceSelector{"matrox", /*fromEnvironment=*/true});
    CHECK(fromEnv.index == 1);
    CHECK(fromEnv.warning.find("MOSAIC_DEVICE") != std::string::npos);
}

TEST_CASE("a selector naming only unusable devices falls back rather than failing") {
    std::vector<DeviceInfo> devices = hybridLaptop();
    devices[1].usable = false;
    devices[1].unusableReason = "no graphics queue family";

    const DeviceChoice choice = pickPhysicalDevice(devices, flagSelector("nvidia"));
    CHECK(choice.index == 0);
    CHECK_FALSE(choice.warning.empty());
    CHECK(choice.warning.find("unusable") != std::string::npos);
}

TEST_CASE("an empty device list yields no choice, with a reason") {
    const DeviceChoice choice = pickPhysicalDevice({}, DeviceSelector{});
    CHECK(choice.index == -1);
    CHECK_FALSE(choice.reason.empty());
    // "No device" means ABSENCE ONLY (settled decision section 10.2). A selector does not change
    // that, and must not turn absence into a different kind of failure.
    CHECK(pickPhysicalDevice({}, flagSelector("nvidia")).index == -1);
}

TEST_CASE("a list of nothing but unusable devices yields no choice") {
    std::vector<DeviceInfo> devices = hybridLaptop();
    devices[0].usable = false;
    devices[1].usable = false;

    const DeviceChoice choice = pickPhysicalDevice(devices, DeviceSelector{});
    CHECK(choice.index == -1);
    CHECK_FALSE(choice.reason.empty());
}

TEST_CASE("a CPU-only list still yields a device, and taking it is visible") {
    // Settled decision section 10.2: a software rasterizer is ACCEPTED IN FULL. It ranks last and
    // it is announced, but it is never a refusal.
    const std::vector<DeviceInfo> devices = {
        dev(0, "llvmpipe (LLVM 17.0.6, 256 bits)", VK_PHYSICAL_DEVICE_TYPE_CPU)};

    const DeviceChoice choice = pickPhysicalDevice(devices, DeviceSelector{});
    CHECK(choice.index == 0);
    CHECK(choice.warning.empty());  // nothing was ignored; this is not a user mistake
    CHECK(choice.reason.find("software rasterizer") != std::string::npos);

    // Naming it explicitly is equally fine.
    CHECK(pickPhysicalDevice(devices, flagSelector("llvmpipe")).index == 0);
}

TEST_CASE("the flag outranks MOSAIC_DEVICE, and silence is representable") {
    // The same precedence gpu_policy.hpp settled for --cpu / MOSAIC_CPU_ONLY: a flag is a one-run
    // override, so it wins.
    const DeviceSelector both = decideDeviceSelector("1", "nvidia");
    CHECK(both.text == "1");
    CHECK_FALSE(both.fromEnvironment);

    const DeviceSelector envOnly = decideDeviceSelector("", "nvidia");
    CHECK(envOnly.text == "nvidia");
    CHECK(envOnly.fromEnvironment);

    const DeviceSelector neither = decideDeviceSelector("", "");
    CHECK(neither.empty());
    CHECK(neither.text.empty());
}

TEST_CASE("the process-wide selector round-trips") {
    const DeviceSelector saved = deviceSelector();
    setDeviceSelector(DeviceSelector{"some-device", /*fromEnvironment=*/true});
    CHECK(deviceSelector().text == "some-device");
    CHECK(deviceSelector().fromEnvironment);
    // Restore: every other GPU case in the suite builds a real device through this selector, and a
    // test that leaves a stale one behind would send them all down the fallback path.
    setDeviceSelector(saved);
    CHECK(deviceSelector().text == saved.text);
}

TEST_CASE("describeDevice prints one pasteable line per device") {
    DeviceInfo d = dev(3, "NVIDIA GeForce RTX 3060", VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    d.apiVersion = VK_API_VERSION_1_3;
    d.vendorId = 0x10DE;
    d.driverName = "NVIDIA";
    d.deviceLocalBytes = 12ull * 1024 * 1024 * 1024;

    const std::string line = describeDevice(d);
    CHECK(line.find("[3]") != std::string::npos);
    CHECK(line.find("NVIDIA GeForce RTX 3060") != std::string::npos);
    CHECK(line.find("discrete") != std::string::npos);
    CHECK(line.find("Vulkan 1.3") != std::string::npos);
    CHECK(line.find("12288 MiB device-local") != std::string::npos);
    CHECK(line.find("unusable") == std::string::npos);

    d.usable = false;
    d.unusableReason = "no graphics queue family";
    const std::string refused = describeDevice(d);
    CHECK(refused.find("[unusable: no graphics queue family]") != std::string::npos);
}

TEST_CASE("driver versions decode per vendor") {
    // NVIDIA's own 10.8.8.6 packing: 535.104.5.0 is what the driver package calls itself, and a
    // Vulkan-layout decode of the same bits prints something that matches nothing.
    CHECK(formatDriverVersion(0x10DE, (535u << 22) | (104u << 14) | (5u << 6)) == "535.104.5.0");
    // Everything else gets the Vulkan layout.
    CHECK(formatDriverVersion(0x1002, VK_MAKE_API_VERSION(0, 23, 1, 5)) == "23.1.5");
}

TEST_CASE("GpuCaps::readout names every negotiated fact") {
    GpuProbe probe;
    applyFloorProfile(probe);
    probe.deviceName = "Floor Device";
    probe.deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;

    const std::string text = decide(probe).readout();
    for (const char* label : {"Device:", "Vulkan:", "Working fmt:", "Tiles:", "Max image:",
                              "Storage bufs:", "Storage imgs:", "Sampled imgs:", "Push consts:",
                              "Compute:", "Timestamps:", "Queues:", "Tiers:"}) {
        CHECK(text.find(label) != std::string::npos);
    }

    CHECK(text.find("Floor Device") != std::string::npos);
    CHECK(text.find("integrated") != std::string::npos);
    CHECK(text.find("rgba16f") != std::string::npos);
    // A floor device has no timestamp counter, and the readout must say that rather than print a
    // zero-width one as though it were usable.
    CHECK(text.find("unsupported") != std::string::npos);
    CHECK(text.back() == '\n');  // every row is terminated, so it concatenates cleanly

    GpuProbe software = probe;
    software.deviceType = VK_PHYSICAL_DEVICE_TYPE_CPU;
    CHECK(decide(software).readout().find("software rasterizer") != std::string::npos);
}
