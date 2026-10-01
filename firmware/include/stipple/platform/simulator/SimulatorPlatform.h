// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "stipple/platform/MqttClient.h"
#include "stipple/platform/simulator/SimulatorHttpClient.h"
#include "stipple/platform/PlatformServices.h"

namespace stipple {
namespace platform {
namespace simulator {

/// The panel, off-device. Keeps the most recent frame so tests and the browser
/// emulator can read back exactly what was presented.
class SimulatorDisplay : public IFrameBufferDisplay {
public:
    void present(const Framebuffer& frame) override;

    void setBrightness(std::uint8_t brightness) override { brightness_ = brightness; }
    std::uint8_t brightness() const override { return brightness_; }

    /// The simulator has no throttle of its own, but it reports the device's
    /// limit anyway. Pacing the emulator to what the TC002 can actually sustain
    /// is the whole point of building animations here first (§9.4).
    int minimumFrameIntervalMillis() const override { return 15; }

    const Framebuffer& lastFrame() const { return lastFrame_; }
    std::uint32_t presentCount() const { return presentCount_; }

private:
    Framebuffer lastFrame_;
    std::uint32_t presentCount_ = 0;
    std::uint8_t brightness_ = 255;
};

/// Input queue fed by the emulator UI or by tests.
///
/// Fixed capacity with oldest-dropped overflow, matching the contract in
/// IInputDevice: a stuck button must not be able to hide later presses, and
/// nothing here may allocate without bound (§38).
class SimulatorInput : public IInputDevice {
public:
    static constexpr std::size_t kCapacity = 32;

    bool poll(InputEvent& event) override;
    std::uint32_t droppedEventCount() const override { return dropped_; }

    /// Inject a raw event, as the hardware would.
    void push(const InputEvent& event);

    /// Convenience for a complete press: Down then Up `durationMillis` apart.
    void pressAndRelease(RawInput source, std::uint64_t downMillis, std::uint64_t durationMillis);

    /// Convenience for one rotary detent.
    void rotate(bool clockwise, std::uint64_t timestampMillis);

    std::size_t pending() const { return size_; }
    void clear();

private:
    InputEvent buffer_[kCapacity];
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint32_t dropped_ = 0;
};

/// A clock the test drives.
///
/// Time only moves when `advance()` is called, which is what lets long-press
/// thresholds, animation timing and app durations be tested exactly and
/// instantly instead of with sleeps and tolerances.
class SimulatorClock : public ISystemClock {
public:
    std::uint64_t monotonicMillis() const override { return monotonicMillis_; }
    bool wallClockValid() const override { return wallClockValid_; }
    std::int64_t unixSeconds() const override { return unixSeconds_; }
    int utcOffsetSeconds() const override { return utcOffsetSeconds_; }

    void advance(std::uint64_t millis);

    /// Starts invalid on purpose, mirroring a device that has booted but not yet
    /// reached NTP.
    void setWallClock(std::int64_t unixSeconds, int utcOffsetSeconds = 0);
    void invalidateWallClock() { wallClockValid_ = false; }

private:
    std::uint64_t monotonicMillis_ = 0;
    std::int64_t unixSeconds_ = 0;
    /// Sub-second remainder, so repeated small advances still add up to whole
    /// seconds instead of being rounded away each time.
    std::uint64_t pendingWallMillis_ = 0;
    int utcOffsetSeconds_ = 0;
    bool wallClockValid_ = false;
};

/// In-memory key/value store. Writes are trivially atomic here; the device
/// implementation is where that has to be earned.
class SimulatorStorage : public IStorage {
public:
    static constexpr std::size_t kMaxValueBytes = 64 * 1024;

    bool exists(std::string_view key) const override;
    bool read(std::string_view key, std::string& out) const override;
    bool write(std::string_view key, std::string_view value) override;
    bool remove(std::string_view key) override;
    std::size_t maxValueBytes() const override { return kMaxValueBytes; }

    std::size_t keyCount() const { return entries_.size(); }
    void clear() { entries_.clear(); }

private:
    std::map<std::string, std::string, std::less<>> entries_;
};

/// Records playback requests; makes no sound.
///
/// Honest about what it is: this verifies that STIPPLE *asked* for a sound at the
/// right moment, which is our logic and worth testing. It says nothing about
/// whether the TC002 speaker works — that is Phase 7's problem.
class SimulatorAudio : public IAudioOutput {
public:
    struct Request {
        bool isTone = false;
        int frequencyHz = 0;
        int durationMillis = 0;
        std::string sound;
    };

    bool playTone(int frequencyHz, int durationMillis) override;
    bool playSound(std::string_view name) override;
    void stop() override;

    void setVolume(std::uint8_t volume) override { volume_ = volume; }
    std::uint8_t volume() const override { return volume_; }

    /// A melody asked for, and the level it was asked at.
    struct MelodyRequest {
        audio::Melody melody;
        int levelPercent = 0;
    };

    bool playMelody(const audio::Melody& melody, int levelPercent) override;
    void stopMelody() override { ++melodyStopCount_; }

    const std::vector<Request>& requests() const { return requests_; }
    const std::vector<MelodyRequest>& melodies() const { return melodies_; }
    std::uint32_t stopCount() const { return stopCount_; }
    std::uint32_t melodyStopCount() const { return melodyStopCount_; }
    void clear() {
        requests_.clear();
        melodies_.clear();
    }

private:
    std::vector<Request> requests_;
    std::vector<MelodyRequest> melodies_;
    std::uint32_t stopCount_ = 0;
    std::uint32_t melodyStopCount_ = 0;
    std::uint8_t volume_ = 128;
};

/// A battery a test drives. Starts unknown, matching a device whose MCU has
/// not answered yet — the state the battery app must handle without inventing
/// a plausible zero.
class SimulatorPower : public IPowerSource {
public:
    BatteryStatus battery() const override { return status_; }

    void setBattery(int percent) {
        status_.known = true;
        status_.percent = percent;
    }
    void forget() { status_ = BatteryStatus{}; }

    /// Drive the charge state independently of the percentage, so a test can
    /// reproduce a device that knows it is plugged in but has no reading yet.
    void setCharging(bool charging) {
        status_.chargingKnown = true;
        status_.charging = charging;
    }

private:
    BatteryStatus status_;
};

/// A microphone a test drives. Silent until told otherwise, and unknown until
/// the capability is switched on - the two states a visualiser must tell apart.
class SimulatorMicrophone : public IMicrophone {
public:
    SoundLevel level() const override { return level_; }

    void hear(int amplitude) {
        level_.known = true;
        level_.amplitude = amplitude;
    }
    void deafen() { level_ = SoundLevel{}; }

private:
    SoundLevel level_;
};

/// Reports whatever status the test sets.
class SimulatorNetwork : public INetworkManager {
public:
    NetworkStatus status() const override { return status_; }
    void setStatus(const NetworkStatus& status) { status_ = status; }

    /// Off by default, matching a simulator that has no radio - so the
    /// "cannot scan" path is the one tests take unless they ask otherwise.
    bool canScan() const override { return scannable_; }
    void setScannable(bool scannable) { scannable_ = scannable; }

    bool beginScan() override {
        if (!scannable_) {
            return false;
        }
        ++scanCount_;
        return true;
    }
    int scanCount() const { return scanCount_; }

    std::vector<WirelessNetwork> networks() const override { return networks_; }
    void setNetworks(std::vector<WirelessNetwork> networks) {
        networks_ = std::move(networks);
    }

private:
    NetworkStatus status_;
    std::vector<WirelessNetwork> networks_;
    bool scannable_ = false;
    int scanCount_ = 0;
};

/// Records the request. Emphatically does not reboot anything.
class SimulatorRebooter : public IRebooter {
public:
    void reboot() override { ++rebootCount_; }
    std::uint32_t rebootCount() const { return rebootCount_; }

private:
    std::uint32_t rebootCount_ = 0;
};

/// An in-memory broker of one.
///
/// Records what the device published and lets a test deliver inbound messages,
/// so the whole MQTT surface is exercisable with no broker, no socket and no
/// network. Connection succeeds or fails on command, which is the only way to
/// test a reconnect policy without waiting for a real outage.
class SimulatorMqtt : public IMqttClient {
public:
    bool connect(const MqttConnectOptions& options, IMqttListener& listener) override;
    void disconnect() override;
    MqttState state() const override { return state_; }
    bool publish(const MqttMessage& message) override;
    bool subscribe(std::string_view topicFilter, int qos) override;
    void poll(std::uint64_t nowMillis) override;

    // --- simulator-only controls --------------------------------------------

    /// Make the next connect() fail to reach the broker. The call still
    /// succeeds — an unreachable broker is a state change, not an argument
    /// error — but the client lands in Disconnected rather than Connected.
    void setReachable(bool reachable) { reachable_ = reachable; }

    /// Deliver a message as though the broker had sent it.
    void deliver(const MqttMessage& message);

    /// Drop the connection, as a broker restart or a lost link would.
    void dropConnection();

    const std::vector<MqttMessage>& published() const { return published_; }
    const std::vector<std::string>& subscriptions() const { return subscriptions_; }
    const MqttConnectOptions& lastConnectOptions() const { return options_; }

    /// Most recent message on a topic, or nullptr. Retained state is what a
    /// subscriber would see, so this is usually what a test wants.
    const MqttMessage* lastOn(std::string_view topic) const;

    void clear() { published_.clear(); }

    /// Refuse further publishes, as a full send queue would. §38 forbids an
    /// unbounded queue, so callers must cope with a refusal.
    void setPublishAccepted(bool accepted) { publishAccepted_ = accepted; }

private:
    void setState(MqttState state);

    IMqttListener* listener_ = nullptr;
    MqttConnectOptions options_;
    MqttState state_ = MqttState::Disabled;
    bool reachable_ = true;
    bool publishAccepted_ = true;
    std::vector<MqttMessage> published_;
    std::vector<std::string> subscriptions_;
};

/// Which optional capabilities this simulated device claims to have.
///
/// Turning one off makes the matching accessor return nullptr, so tests can
/// exercise the path where a platform genuinely lacks a capability. Without
/// this the nullptr branches would never run off-device and would first be
/// discovered on real hardware.
struct SimulatorCapabilities {
    bool audio = true;
    bool network = true;
    bool rebooter = true;
    bool mqtt = true;
    /// Off by default: most panels are mains-only, and a simulator that always
    /// claimed a battery would hide the nullptr path from every test.
    bool power = false;
    /// Off by default for the same reason.
    bool microphone = false;

    /// Outbound HTTP for scripts.
    ///
    /// Off by default, like every other capability here, so a test that does
    /// not ask for one gets a device that honestly has none - which is the
    /// configuration the "says so when it cannot" tests need.
    bool httpClient = false;
};

/// Complete simulator implementation of the §53 platform boundary.
class SimulatorPlatform : public IPlatformServices {
public:
    explicit SimulatorPlatform(const SimulatorCapabilities& capabilities = SimulatorCapabilities{})
        : capabilities_(capabilities) {}

    const char* name() const override { return "simulator"; }

    IFrameBufferDisplay& display() override { return display_; }
    IInputDevice& input() override { return input_; }
    ISystemClock& clock() override { return clock_; }
    IStorage& storage() override { return storage_; }

    IAudioOutput* audio() override { return capabilities_.audio ? &audio_ : nullptr; }
    INetworkManager* network() override { return capabilities_.network ? &network_ : nullptr; }
    IRebooter* rebooter() override { return capabilities_.rebooter ? &rebooter_ : nullptr; }
    IPowerSource* power() override { return capabilities_.power ? &power_ : nullptr; }
    IMicrophone* microphone() override {
        return capabilities_.microphone ? &microphone_ : nullptr;
    }
    IMqttClient* mqtt() override { return capabilities_.mqtt ? &mqtt_ : nullptr; }
    IHttpClient* httpClient() override {
        return capabilities_.httpClient ? &httpClient_ : nullptr;
    }

    // Concrete accessors for tests and the emulator shell, which need the
    // simulator-only controls that the interfaces deliberately do not expose.
    SimulatorDisplay& simulatedDisplay() { return display_; }
    SimulatorInput& simulatedInput() { return input_; }
    SimulatorClock& simulatedClock() { return clock_; }
    SimulatorStorage& simulatedStorage() { return storage_; }
    SimulatorAudio& simulatedAudio() { return audio_; }
    SimulatorNetwork& simulatedNetwork() { return network_; }
    SimulatorRebooter& simulatedRebooter() { return rebooter_; }
    SimulatorPower& simulatedPower() { return power_; }
    SimulatorMicrophone& simulatedMicrophone() { return microphone_; }
    SimulatorMqtt& simulatedMqtt() { return mqtt_; }
    SimulatorHttpClient& simulatedHttpClient() { return httpClient_; }

private:
    SimulatorCapabilities capabilities_;
    SimulatorDisplay display_;
    SimulatorInput input_;
    SimulatorClock clock_;
    SimulatorStorage storage_;
    SimulatorAudio audio_;
    SimulatorNetwork network_;
    SimulatorPower power_;
    SimulatorMicrophone microphone_;
    SimulatorMqtt mqtt_;
    SimulatorHttpClient httpClient_;
    SimulatorRebooter rebooter_;
};

}  // namespace simulator
}  // namespace platform
}  // namespace stipple
