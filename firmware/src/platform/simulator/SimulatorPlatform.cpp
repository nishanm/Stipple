// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/simulator/SimulatorPlatform.h"

namespace stipple {
namespace platform {
namespace simulator {

// --- display ----------------------------------------------------------------

void SimulatorDisplay::present(const Framebuffer& frame) {
    lastFrame_ = frame;

    // Brightness is applied here because that is where the real panel applies
    // it. Core renders in true colour and never has to know the current
    // setting; doing it in the adapter keeps the simulator honest about what
    // the hardware will actually show.
    if (brightness_ != 255) {
        Rgb* pixels = lastFrame_.data();
        for (int i = 0; i < Framebuffer::kPixelCount; ++i) {
            pixels[i] = scale(pixels[i], brightness_);
        }
    }

    ++presentCount_;
}

// --- input ------------------------------------------------------------------

void SimulatorInput::push(const InputEvent& event) {
    if (size_ == kCapacity) {
        // Overflow drops the OLDEST event, per the IInputDevice contract:
        // dropping the newest would let one stuck control mask every press
        // that follows it.
        head_ = (head_ + 1) % kCapacity;
        --size_;
        ++dropped_;
    }
    buffer_[(head_ + size_) % kCapacity] = event;
    ++size_;
}

bool SimulatorInput::poll(InputEvent& event) {
    if (size_ == 0) {
        return false;
    }
    event = buffer_[head_];
    head_ = (head_ + 1) % kCapacity;
    --size_;
    return true;
}

void SimulatorInput::pressAndRelease(RawInput source,
                                     std::uint64_t downMillis,
                                     std::uint64_t durationMillis) {
    push(InputEvent{source, ButtonPhase::Down, downMillis});
    push(InputEvent{source, ButtonPhase::Up, downMillis + durationMillis});
}

void SimulatorInput::rotate(bool clockwise, std::uint64_t timestampMillis) {
    push(InputEvent{clockwise ? RawInput::RotaryRight : RawInput::RotaryLeft, ButtonPhase::Tick,
                    timestampMillis});
}

void SimulatorInput::clear() {
    head_ = 0;
    size_ = 0;
}

// --- clock ------------------------------------------------------------------

void SimulatorClock::advance(std::uint64_t millis) {
    monotonicMillis_ += millis;
    if (wallClockValid_) {
        // Keep the wall clock consistent with elapsed time; only whole seconds
        // move, so sub-second advances accumulate rather than rounding away.
        const std::uint64_t totalMillis = pendingWallMillis_ + millis;
        unixSeconds_ += static_cast<std::int64_t>(totalMillis / 1000u);
        pendingWallMillis_ = totalMillis % 1000u;
    }
}

void SimulatorClock::setWallClock(std::int64_t unixSeconds, int utcOffsetSeconds) {
    unixSeconds_ = unixSeconds;
    utcOffsetSeconds_ = utcOffsetSeconds;
    pendingWallMillis_ = 0;
    wallClockValid_ = true;
}

// --- storage ----------------------------------------------------------------

bool SimulatorStorage::exists(std::string_view key) const {
    return entries_.find(key) != entries_.end();
}

bool SimulatorStorage::read(std::string_view key, std::string& out) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        out.clear();
        return false;
    }
    out = it->second;
    return true;
}

bool SimulatorStorage::write(std::string_view key, std::string_view value) {
    if (key.empty() || value.size() > kMaxValueBytes) {
        return false;
    }
    entries_[std::string(key)] = std::string(value);
    return true;
}

bool SimulatorStorage::remove(std::string_view key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    entries_.erase(it);
    return true;
}

// --- audio ------------------------------------------------------------------

bool SimulatorAudio::playTone(int frequencyHz, int durationMillis) {
    if (frequencyHz <= 0 || durationMillis <= 0) {
        return false;
    }
    Request request;
    request.isTone = true;
    request.frequencyHz = frequencyHz;
    request.durationMillis = durationMillis;
    requests_.push_back(request);
    return true;
}

bool SimulatorAudio::playSound(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    Request request;
    request.isTone = false;
    request.sound = std::string(name);
    requests_.push_back(request);
    return true;
}

void SimulatorAudio::stop() {
    ++stopCount_;
}

bool SimulatorAudio::playMelody(const audio::Melody& melody, int levelPercent) {
    if (melody.count <= 0) {
        return false;
    }
    MelodyRequest request;
    request.melody = melody;
    request.levelPercent = levelPercent;
    melodies_.push_back(request);
    return true;
}

// --- MQTT --------------------------------------------------------------------

void SimulatorMqtt::setState(MqttState state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    if (listener_ != nullptr) {
        listener_->onStateChanged(state_);
    }
}

bool SimulatorMqtt::connect(const MqttConnectOptions& options, IMqttListener& listener) {
    // A broker that cannot be reached is reported through the state, not the
    // return value: the caller's reconnect policy needs to see an attempt that
    // was made and failed, not one that was never started.
    if (options.host.empty()) {
        return false;
    }

    listener_ = &listener;
    options_ = options;

    setState(MqttState::Connecting);
    setState(reachable_ ? MqttState::Connected : MqttState::Disconnected);
    return true;
}

void SimulatorMqtt::disconnect() {
    subscriptions_.clear();
    setState(MqttState::Disabled);
    listener_ = nullptr;
}

bool SimulatorMqtt::publish(const MqttMessage& message) {
    if (state_ != MqttState::Connected || !publishAccepted_) {
        return false;
    }
    published_.push_back(message);
    return true;
}

bool SimulatorMqtt::subscribe(std::string_view topicFilter, int qos) {
    (void)qos;
    if (state_ != MqttState::Connected) {
        return false;
    }
    subscriptions_.emplace_back(topicFilter);
    return true;
}

void SimulatorMqtt::poll(std::uint64_t nowMillis) {
    (void)nowMillis;  // nothing arrives on its own; tests call deliver()
}

void SimulatorMqtt::deliver(const MqttMessage& message) {
    if (listener_ != nullptr && state_ == MqttState::Connected) {
        listener_->onMessage(message);
    }
}

void SimulatorMqtt::dropConnection() {
    subscriptions_.clear();
    setState(MqttState::Disconnected);
}

const MqttMessage* SimulatorMqtt::lastOn(std::string_view topic) const {
    for (std::size_t i = published_.size(); i > 0; --i) {
        const MqttMessage& message = published_[i - 1];
        if (message.topic == topic) {
            return &message;
        }
    }
    return nullptr;
}

}  // namespace simulator
}  // namespace platform
}  // namespace stipple
