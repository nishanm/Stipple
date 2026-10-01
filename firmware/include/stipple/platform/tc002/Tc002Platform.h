// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "stipple/platform/PlatformServices.h"
#include "stipple/platform/tc002/Tc002Dhcp.h"
#include "stipple/platform/tc002/Tc002Sntp.h"
#include "stipple/platform/tc002/Tc002Hotspot.h"
#include "stipple/platform/tc002/Tc002Upgrade.h"
#include "stipple/platform/tc002/Tc002Display.h"
#include "stipple/platform/tc002/Tc002HttpServer.h"
#include "stipple/platform/tc002/Tc002Input.h"
#include "stipple/platform/tc002/Tc002Audio.h"
#include "stipple/platform/tc002/Tc002Mcu.h"
#include "stipple/platform/tc002/WpaControl.h"
#include "stipple/platform/tc002/Tc002HttpClient.h"
#include "stipple/platform/tc002/Tc002MqttClient.h"

namespace stipple {
namespace platform {
namespace tc002 {

/// Time on a device with no RTC.
///
/// `zkgui` logs "open /dev/rtc0 fail" on every boot, and there is no battery to
/// keep one running anyway, so the wall clock is meaningless until something
/// sets it. That is exactly the case ISystemClock::wallClockValid() exists for:
/// a clock face that trusted CLOCK_REALTIME on a cold boot would confidently
/// render 1970.
class Tc002Clock final : public ISystemClock {
public:
    /// Anything earlier than this is a kernel default rather than a real time.
    /// 2020-01-01T00:00:00Z — comfortably after any plausible build date and
    /// far enough from the epoch that an unset clock cannot be mistaken for a
    /// set one.
    static constexpr std::int64_t kPlausibleEpoch = 1577836800;

    std::uint64_t monotonicMillis() const override;
    bool wallClockValid() const override;
    std::int64_t unixSeconds() const override;

    /// Zero. The device carries no tzdata, so there is nothing to read an
    /// offset from — it belongs in configuration, where the user sets it, not
    /// in a platform guess.
    int utcOffsetSeconds() const override { return 0; }
};

/// Key/value storage on /data.
///
/// /res is a read-only squashfs, so this is the only writable place on the
/// device — 7.6 MB free, which is ample for configuration and an icon set.
///
/// Atomicity is earned rather than assumed, per IStorage: each write goes to a
/// temporary file, is fsynced, then renamed over the target, and the directory
/// is fsynced after. rename(2) within a filesystem is atomic, so a power cut
/// leaves either the old value or the new one and never a torn mixture. The
/// simulator gets this for free; here it is the whole implementation.
class Tc002Storage final : public IStorage {
public:
    static constexpr std::size_t kMaxValueBytes = 64 * 1024;

    explicit Tc002Storage(std::string directory = "/data/stipple");

    /// Creates the directory if absent. Returns false if it cannot be used,
    /// which the host treats as an unusable platform rather than limping on
    /// with settings that will not persist.
    bool open();

    bool exists(std::string_view key) const override;
    bool read(std::string_view key, std::string& out) const override;
    bool write(std::string_view key, std::string_view value) override;
    bool remove(std::string_view key) override;
    std::size_t maxValueBytes() const override { return kMaxValueBytes; }

    const std::string& directory() const noexcept { return directory_; }

private:
    /// Builds a path, or returns empty if the key is not a safe filename.
    ///
    /// Keys are internal today ("boot", "icons", config), but §23 says API
    /// input must never name a path, and the cheapest way to keep that true is
    /// for this layer to refuse anything that could become one.
    std::string pathFor(std::string_view key) const;

    std::string directory_;
};

/// Wi-Fi state, read-only.
///
/// getifaddrs(3) rather than a shell: the device busybox is missing enough
/// commands — no grep, no sleep, no readlink — that shelling out is a liability,
/// and this is netlink-based so it works in a static binary where NSS does not.
class Tc002Network final : public INetworkManager {
public:
    NetworkStatus status() const override;

    /// True once the supplicant's control socket answers. False means it is
    /// not running, which is a real state on a device that has been put into
    /// hotspot mode - not an error, and not "no networks in range".
    bool canScan() const override;

    bool beginScan() override;
    std::vector<WirelessNetwork> networks() const override;

    bool networksAreLive() const override { return live_; }

    bool canJoin() const override;
    bool beginJoin(const std::string& ssid, const std::string& password) override;

    bool canRemember() const override;
    std::vector<RememberedNetwork> rememberedNetworks() const override;
    bool rememberNetwork(const std::string& ssid, const std::string& password,
                         std::string& why) override;
    bool forgetNetwork(const std::string& ssid, std::string& why) override;
    JoinProgress joinProgress() const override;

    /// Drive the join, once a frame. Nothing here blocks.
    ///
    /// Joining is the longest-running thing this device does - a stopped
    /// hotspot, a restarted supplicant, an association and a lease, tens of
    /// seconds end to end - so it is a state machine polled from the loop
    /// like the MCU and the transports, not a call that waits.
    void poll(std::uint64_t nowMillis);

    /// What the hotspot is, so a join can take the radio back off it.
    ///
    /// A join arriving over the hotspot has to shut that hotspot down before
    /// it can do anything, which is also why beginJoin returns before the
    /// work starts.
    void observe(Tc002Hotspot* hotspot) noexcept { hotspot_ = hotspot; }

    /// Where the lease comes from, so status() can report it.
    ///
    /// A pointer rather than ownership: the client belongs to the platform
    /// and is pumped from the loop, while this is the read-only view of it
    /// that core is allowed to see. Null means nothing is managing a lease,
    /// which status() reports as such rather than as zero seconds left.
    void observe(const Tc002Dhcp* dhcp) noexcept { dhcp_ = dhcp; }

private:
    /// What a join is doing. Kept out of the header's public face because
    /// callers ask through joinProgress(), which reports it in words.
    enum class Stage {
        Idle,
        /// Waiting a moment so the HTTP reply is out before the radio moves.
        Settling,
        /// Hotspot down, waiting for the supplicant to answer again.
        Restoring,
        /// Writing the network block.
        Configuring,
        /// Waiting for the association.
        Associating,
        /// Associated; waiting for an address, which is what proves it.
        Addressing,
        Done,
        Failed,
    };

    void fail(const std::string& why);
    void forgetAddedNetwork();
    bool configureNetwork();

    const Tc002Dhcp* dhcp_ = nullptr;
    Tc002Hotspot* hotspot_ = nullptr;

    /// The last scan, kept so it can still be shown while the radio is busy
    /// being an access point.
    mutable std::vector<WirelessNetwork> remembered_;
    mutable bool live_ = true;

    Stage stage_ = Stage::Idle;
    std::string joinSsid_;
    std::string joinPassword_;
    std::string joinDetail_;
    std::uint64_t stageDeadlineMillis_ = 0;
    int addedNetworkId_ = -1;
    bool askedForAddress_ = false;

    /// Opened on first use and kept.
    ///
    /// Mutable because status() and networks() are const - they observe the
    /// device rather than change it - while the socket underneath is not. The
    /// alternative is a non-const interface for reading, which would be worse
    /// documentation of what these calls actually do.
    mutable WpaControl control_;

    /// Ensure the socket is connected, or say it cannot be.
    bool connected() const;
};

/// The TC002 half of the §53 boundary.
///
/// Optional capabilities report absence honestly, per ADR 0013, rather than
/// accepting calls and doing nothing:
///
///   audio     — /dev/mi_ao exists and the SigmaStar MI layer drives it, but
///               nothing here speaks that protocol yet.
///   rebooter  — deliberately absent until there is a reason to expose the most
///               destructive thing STIPPLE can do to a clock.
/// MQTT is always present: unlike audio, "no broker configured" is a state the
/// service already models, so reporting the capability as absent would be the
/// wrong answer.
class Tc002Platform final : public IPlatformServices {
public:
    /// Tell the vendor's recovery daemon that the application is alive.
    ///
    /// `/bin/zkdaemon` polls the property `sys.zkapp.state` and, if it has
    /// not become "running" within `ZK_APPCHECK_DELAY`, does what its own
    /// strings call auto recovery:
    ///
    ///     '[D][zkdaemon] Auto recovery triggered'
    ///     'setprop ctl.stop zkswe'    'rm -rf /data/*'
    ///     '/mnt/storage'  '%s/update.img'  '/bin/zkupgradebin'
    ///
    /// The stock application sets the property - `libzkgui.so` carries the
    /// string, `zkdaemon` carries the check. **STIPPLE replaces that
    /// application, so without this the device deletes STIPPLE, deletes the
    /// Wi-Fi credentials sitting beside it in /data, and reinstalls whatever
    /// image happens to be staged.**
    ///
    /// That is not hypothetical. It is what happened to the first flashed
    /// build, and from the outside it looked like a mysterious revert with a
    /// progress bar - see docs/research/tc002-platform-findings.md.
    void announceRunning() const;

    /// Points the read-only network view at the lease. A fact about how this
    /// object is assembled, not about whether any device opened, so it
    /// belongs here rather than in open().
    Tc002Platform() {
        network_.observe(&dhcp_);
        network_.observe(&hotspot_);
        // The hotspot cannot give the radio back without this: restoring
        // wpa_supplicant gets an association, and nothing else on this
        // device turns an association into an address.
        hotspot_.useDhcp(&dhcp_);
    }

    /// Brings up display, input, storage and clock. Returns false if the
    /// display or input cannot be opened; those are required services and a
    /// clock without them is not worth starting.
    bool open();
    void close() noexcept;

    const char* name() const override { return "tc002"; }

    IFrameBufferDisplay& display() override { return display_; }
    IInputDevice& input() override { return input_; }
    ISystemClock& clock() override { return clock_; }
    IStorage& storage() override { return storage_; }

    INetworkManager* network() override { return &network_; }

    /// Always present on hardware: the storage volume the loader reads is
    /// always there, whether or not anything has been staged on it.
    IUpgradeManager* upgrade() override { return &upgrade_; }

    /// Non-null only once the MCU link is open. A device whose serial port
    /// could not be configured reports no battery rather than zero percent.
    IPowerSource* power() override { return mcu_.isOpen() ? &mcu_ : nullptr; }

    /// Same link, same poll: the MCU carries both battery and microphone.
    IMicrophone* microphone() override { return mcu_.isOpen() ? &mcu_ : nullptr; }

    /// Non-null only once the vendor audio library has loaded and accepted a
    /// configuration. A build that cannot dlopen - a static one - reports no
    /// speaker rather than accepting sounds it will never make (ADR 0013).
    IAudioOutput* audio() override { return audio_.isOpen() ? &audio_ : nullptr; }

    /// Non-null once start() has been called on it. Reported through the
    /// interface so core sees a transport appear exactly when one exists.
    IHttpServer* httpServer() override {
        return http_.running() ? &http_ : nullptr;
    }

    /// Always offered: a configured-but-disconnected broker is a state the
    /// service reports, not an absent capability.
    IMqttClient* mqtt() override { return &mqtt_; }
    IHttpClient* httpClient() override { return &httpClient_; }

    Tc002Display& panel() noexcept { return display_; }

    /// Concrete, because the MCU is polled from the loop like the transport.
    Tc002Mcu& mcu() noexcept { return mcu_; }

    /// Concrete, because audio is fed from the loop a frame at a time rather
    /// than queued: §16 says it must never block rendering, and a second of
    /// sound is a hundred and twenty frames.
    Tc002Audio& audio_out() noexcept { return audio_; }

    /// Concrete, because the transport is polled rather than threaded and
    /// IHttpServer has no poll() — see Tc002HttpServer for why.
    Tc002HttpServer& http() noexcept { return http_; }

    /// Concrete, and not behind INetworkManager, because holding a lease is
    /// not something core should be able to ask for or turn off. It is what
    /// makes the device reachable at all, on a platform that has nothing else
    /// able to do it.
    Tc002Dhcp& dhcp() noexcept { return dhcp_; }

    /// Concrete, and owned here rather than by a separate tool.
    ///
    /// It used to live in one, and both live tests failed on the tool's
    /// lifetime rather than on anything about hosting: the revert depended on
    /// a detached process nobody was watching staying alive, and when it
    /// stopped the radio was left with no access point and no station. This
    /// process survives ADB dropping - that is what ignoring SIGHUP is for -
    /// and already calls tick() every frame, so the deadline is enforced by
    /// something that is definitely still running.
    Tc002Hotspot& hotspot() noexcept { return hotspot_; }

    /// Concrete, because joining is polled from the loop and
    /// INetworkManager has no poll() - the interface describes what core is
    /// allowed to ask for, not how the adapter keeps its promises.
    Tc002Network& wifi() noexcept { return network_; }

    /// The time source. Nothing else on this device sets the clock.
    Tc002Sntp& sntp() noexcept { return sntp_; }

private:
    Tc002Display display_;
    Tc002Input input_;
    Tc002Clock clock_;
    Tc002Storage storage_;
    Tc002Network network_;
    Tc002Mcu mcu_;
    Tc002Audio audio_;
    Tc002MqttClient mqtt_;
    /// Outbound fetches for scripts. Not to be confused with `http_` below,
    /// which is the server this device *answers* on.
    Tc002HttpClient httpClient_;
    Tc002HttpServer http_;
    Tc002Dhcp dhcp_;
    Tc002Sntp sntp_;
    Tc002Hotspot hotspot_;
    Tc002Upgrade upgrade_;
};

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
