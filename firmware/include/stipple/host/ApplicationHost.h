// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string_view>

#include "stipple/api/ApiServer.h"
#include "stipple/web/StaticFiles.h"
#include "stipple/app/Carousel.h"
#include "stipple/asset/IconStore.h"
#include "stipple/script/IScriptRunner.h"
#include "stipple/apps/ClockApp.h"
#include "stipple/apps/GlucoseModel.h"
#include "stipple/apps/VisualizerApp.h"
#include "stipple/apps/SplashScreen.h"
#include "stipple/config/Config.h"
#include "stipple/core/Log.h"
#include "stipple/graphics/Framebuffer.h"
#include "stipple/input/InputMapper.h"
#include "stipple/input/Navigator.h"
#include "stipple/input/Rescue.h"
#include "stipple/apps/StopwatchApp.h"
#include "stipple/input/SetupHold.h"
#include "stipple/json/Json.h"
#include "stipple/mqtt/MqttService.h"
#include "stipple/net/ScriptFetcher.h"
#include "stipple/notify/Notifications.h"
#include "stipple/platform/HttpServer.h"
#include "stipple/platform/PlatformServices.h"
#include "stipple/render/FrameScheduler.h"
#include "stipple/render/Transition.h"
#include "stipple/scene/Scene.h"
#include "stipple/time/Timezone.h"

namespace stipple {
namespace host {

enum class BootMode : std::uint8_t {
    Normal,
    /// Reached after repeated failed boots. Defaults only, no stored apps or
    /// settings, and a screen that says so.
    SafeMode,
};

const char* bootModeName(BootMode mode) noexcept;

/// Persisted across reboots to detect a device that cannot get through startup.
struct BootRecord {
    std::uint32_t consecutiveFailures = 0;
    /// False while a boot is in progress. If it is still false on the next
    /// startup, the previous attempt never finished.
    bool lastBootCompleted = true;
};

struct HostConfig {
    /// Consecutive unfinished boots before falling back to safe mode.
    int safeModeThreshold = 3;

    /// Frames that must render before a boot counts as healthy. More than one,
    /// so a crash on the second frame still counts as a failure.
    int healthyAfterFrames = 3;

    /// ...or this long spent ticking without incident, whichever comes first.
    ///
    /// A frame count alone is not enough once dirty rendering exists: a static
    /// clock face legitimately renders once and then nothing. Without this, such
    /// a device would never record a healthy boot, every restart would count as
    /// a failure, and it would fall into safe mode permanently — the opposite of
    /// what the anti-brick mechanism is for.
    std::uint32_t healthyAfterMillis = 3000;

    render::FrameScheduler::Config frame;
    api::ApiOptions api;
    apps::ClockStyle clock;
    apps::SplashStyle splash;

    /// How long the boot splash stays up. Long enough for a scrolling IP
    /// address to finish at least once; zero disables it. Any button press
    /// dismisses it early.
    /// Ten seconds, split into two pages of five.
    ///
    /// Longer than it looks like it needs to be, on purpose. The second page
    /// reports the address, and the radio takes ten to twenty seconds to
    /// associate from cold - so a shorter splash guarantees the one screen
    /// that talks about the network is drawn before there is a network to
    /// talk about. Five seconds of wave first is what buys that time.
    std::uint32_t splashMillis = 10000;

    /// Register the built-in clock so a fresh device shows something.
    bool installClockApp = true;
};

/// Owns startup, the main loop and shutdown (blueprint §8.1).
///
/// The loop is `tick(now)` rather than a blocking `run()`. Core has no business
/// sleeping or owning a thread — the browser drives this from
/// requestAnimationFrame and the device will drive it from its own loop, with
/// `nextDueMillis()` telling each how long it may wait. Same code, no threading
/// primitives anywhere near the renderer.
///
/// The anti-brick guarantee (§8.1, §21) is the part that matters most before
/// hardware exists: a boot marker is written before startup and only cleared
/// once frames are actually rendering. Repeated failures bring the device up in
/// safe mode with default settings and no stored apps, so a bad configuration or
/// a poisonous app cannot leave a clock that has to be opened up to recover.
class ApplicationHost : public platform::IHttpRequestHandler, public platform::IInputSink {
public:
    static constexpr std::string_view kBootStateKey = "boot";
    static constexpr std::string_view kClockAppId = "clock";
    static constexpr std::string_view kBatteryAppId = "battery";
    static constexpr std::string_view kVisualizerAppId = "visualizer";
    static constexpr std::string_view kStopwatchAppId = "stopwatch";
    static constexpr std::string_view kGlucoseAppId = "glucose";
    static constexpr std::string_view kIconStateKey = "icons";
    static constexpr std::string_view kScriptStateKey = "scripts";
    static constexpr int kSceneTokens = 512;

    ApplicationHost(platform::IPlatformServices& platform, HostConfig config = HostConfig{});

    ApplicationHost(const ApplicationHost&) = delete;
    ApplicationHost& operator=(const ApplicationHost&) = delete;

    /// Run the boot sequence. Returns false only if the platform is unusable.
    bool initialize();

    /// One iteration: input, scheduling, and a frame if one is due. Returns
    /// false once shutdown has been requested.
    bool tick(std::uint64_t nowMillis);

    /// Earliest time `tick` needs calling again. The caller sleeps until then.
    std::uint64_t nextDueMillis(std::uint64_t nowMillis) const;

    void shutdown();

    // platform::IHttpRequestHandler
    api::Response handle(const api::Request& request) override;

    BootMode bootMode() const noexcept { return bootMode_; }
    bool initialized() const noexcept { return initialized_; }

    /// True once enough frames have rendered for the boot to be recorded as
    /// successful.
    bool healthy() const noexcept { return healthy_; }

    const BootRecord& bootRecord() const noexcept { return bootRecord_; }

    log::RingLog& logger() noexcept { return logger_; }
    const log::RingLog& logger() const noexcept { return logger_; }

    app::AppRegistry& apps() noexcept { return registry_; }
    app::Carousel& carousel() noexcept { return carousel_; }
    notify::NotificationQueue& notifications() noexcept { return notifications_; }
    asset::IconStore& icons() noexcept { return icons_; }

    /// Give the host something that can run scripts.
    ///
    /// Optional, and null is a supported configuration rather than a broken
    /// one - the core cannot link a language runtime (ADR 0012), so whoever
    /// builds the platform decides whether scripting exists. A host without a
    /// runner still shows script apps; it shows them saying scripting is not
    /// available, which is what ADR 0013 asks for.
    ///
    /// The host does not own it. It outlives the host in every arrangement
    /// that exists - main() holds it, the simulator holds it, a test holds it
    /// on the stack.
    void setScriptRunner(script::IScriptRunner* runner);
    script::IScriptRunner* scriptRunner() const noexcept { return scripts_; }
    config::Config& settings() noexcept { return settings_; }

    /// Exposed so a settings UI can show what the controls currently do, rather
    /// than hard-coding a copy of the default mapping that then drifts.
    const input::InputMapper& inputMapper() const noexcept { return mapper_; }

    /// Which mode the physical controls are pointed at, and what they are
    /// pointed at inside it (ADR 0017).
    const input::Navigator& navigator() const noexcept { return navigator_; }

    /// The rescue gesture, for the renderer that draws its countdown and for
    /// tests that drive it.
    const input::Rescue& rescue() const noexcept { return rescue_; }

    /// The knob-hold gesture, for the countdown renderer and for tests.
    const input::SetupHold& setupHold() const noexcept { return setupHold_; }

    /// True once, for the caller that starts the hotspot.
    ///
    /// Deliberately not the persisted `hotspotRequested` flag. That one is
    /// only honoured on a device with no address, because honouring it
    /// unconditionally once made a rescued device host a setup network on
    /// every boot. A hold means "host one now", on a device that may be
    /// perfectly online - a different question needing a different channel,
    /// and one that must not survive a reboot.
    bool takeSetupRequest() noexcept {
        const bool asked = setupRequested_;
        setupRequested_ = false;
        return asked;
    }

    mqtt::MqttService& mqttService() noexcept { return mqtt_; }
    const mqtt::MqttService& mqttService() const noexcept { return mqtt_; }

    /// What scripts fetch, and how often. Exposed for the diagnostics that
    /// answer "is this device hammering my API".
    net::ScriptFetcher& scriptFetcher() noexcept { return fetcher_; }
    const net::ScriptFetcher& scriptFetcher() const noexcept { return fetcher_; }
    render::FrameScheduler& scheduler() noexcept { return scheduler_; }
    const render::FrameStats& frameStats() const noexcept { return scheduler_.stats(); }

    /// The most recently rendered frame.
    const Framebuffer& frame() const noexcept { return framebuffer_; }

    /// Clock style with the user's stored preferences applied over the host
    /// defaults, so a settings change takes effect on the next frame without any
    /// extra wiring. Public because the resolved style — not the raw strings in
    /// config — is what a settings UI needs to show what is actually in force.
    apps::ClockStyle clockStyle() const noexcept;

    /// Feed a raw hardware event. Normally the platform's input queue supplies
    /// these; exposed so a host can inject them directly.
    void handleInput(const platform::InputEvent& event);

    // platform::IInputSink
    //
    // The web UI's on-screen controls arrive here, through exactly the path the
    // physical ones take: same mapper, same long-press timing, same splash
    // dismissal. Anything less and a browser would be exercising behaviour the
    // panel does not have.
    void inject(const platform::InputEvent& event) override { handleInput(event); }

    /// Show a full-screen notice instead of everything else, until cleared.
    ///
    /// **The panel is the only channel that still works when the network is
    /// the thing that is broken**, which is exactly when this is needed: a
    /// device hosting an access point whose name nobody can see is a device
    /// nobody can reach. Two live hotspot tests were diagnosed entirely from
    /// the outside because the panel said nothing.
    ///
    /// Core does not know what a hotspot is and does not need to - it is
    /// given two lines and shows them. The detail line scrolls, because an
    /// address truncated to "192..." helps nobody.
    void setNotice(std::string title, std::string detail);
    void clearNotice() noexcept;
    bool showingNotice() const noexcept { return !noticeTitle_.empty(); }

    /// Nothing was stored when this device started: it has never been set up.
    ///
    /// A state, not a wizard (ADR 0018). Nothing about the device behaves
    /// differently because of it - the clock still runs, the apps still
    /// rotate - but the panel explains itself and the configuration page
    /// opens on the step that matters instead of on the live view.
    ///
    /// It stops being true the moment anything is saved, which is the first
    /// thing a person does. There is no "finish setup" button, because a
    /// button somebody has to find is a step that can be missed.
    bool firstRun() const noexcept { return firstRun_; }

    /// Forget that setup mode was asked for, and persist that.
    ///
    /// ADR 0018 always said the flag is cleared once a real network is
    /// joined. It was set and persisted and never cleared, so a single use
    /// of the rescue gesture turned into a device that hosted a setup
    /// network on every boot from then on - seen on real hardware, where it
    /// looked like a crash.
    ///
    /// Not exposed through the API: it is internal state, not a setting
    /// somebody should be able to toggle from a page that may itself only be
    /// reachable because the flag is set.
    void clearHotspotRequest();

    /// True while the boot splash is still showing.
    bool showingSplash() const noexcept { return splashActive_; }

    /// Cut the splash short — any button press does this.
    void dismissSplash() noexcept;

private:
    api::ApiContext makeContext() noexcept;

    BootRecord readBootRecord();
    void writeBootRecord(const BootRecord& record);
    void markHealthy();

    void installBuiltins();
    void loadIcons();
    void loadScripts();
    /// Writes the icon set if it has changed since the last save. Called from
    /// tick(), so every mutation path is covered rather than just the API.
    void persistIconsIfChanged();
    void persistScriptsIfChanged();
    void publishScriptEnvironment();
    void pumpInput(std::uint64_t nowMillis);
    void renderFrame(std::uint64_t nowMillis);
    void renderSafeMode();
    bool refreshActiveScene();

    /// Start a transition from whatever is currently on screen.
    void beginTransition(std::uint64_t nowMillis, render::TransitionDirection direction);
    /// True while one is still running at `nowMillis`.
    bool transitionRunning(std::uint64_t nowMillis) const noexcept;

    bool splashElapsed(std::uint64_t nowMillis) const noexcept;

    platform::IPlatformServices& platform_;
    HostConfig config_;

    log::RingLog logger_;
    config::ConfigStore configStore_;
    config::Config settings_;

    app::AppRegistry registry_;
    app::Carousel carousel_;
    notify::NotificationQueue notifications_;
    asset::IconStore icons_;
    script::IScriptRunner* scripts_ = nullptr;
    std::uint32_t persistedScriptRevision_ = 0;
    /// Move brightness by `steps` of the configured step size, clamped, and
    /// bring the panel back on if it was off.
    void adjustBrightness(int steps);

    /// Move volume by `steps`. Returns false when the platform has no speaker,
    /// which is what lets settings hide the control rather than offer a dead
    /// one (ADR 0013).
    bool adjustVolume(int steps);

    /// Apply an adjustment to whatever the navigator has selected.
    void adjustCurrentSetting(int steps);

    /// The knob press, inside settings: toggles what can be toggled.
    void activateCurrentSetting();

    /// Re-parse the timezone rule when it changes, and say so in the log.
    void applyTimeSettings();

    /// Seconds to add to UTC right now, from the timezone rule where one is
    /// configured and from the stored offset where it is not.
    int currentUtcOffsetSeconds() const;

    /// Clear the way back in: access password gone, hotspot requested.
    void performRescue();

    /// Ask for a hotspot and change nothing else.
    void performSetupRequest();


    /// Whether the overnight dimming window applies right now.
    bool nightModeActive() const;

    /// Push whichever brightness should be in force to the panel.
    void applyBrightness();

    /// Keep the carousel in step with the stored app settings.
    void applyCarouselSettings();

    /// Put the registry into the order the user last arranged, and apply the
    /// enabled flags and durations that went with it.
    void applyStoredAppOrder();

    /// Whether the stored order still describes the live registry. False
    /// means the settings changed from outside - a restored backup, say.
    bool storedOrderMatchesRegistry() const;

    /// Write the order out when the registry says it changed, and apply it
    /// when the settings changed instead.
    void persistAppOrderIfChanged();

    /// Copy the registry's current order back into settings, ready to persist.
    void rememberAppOrder();

    /// Play a notification's sound, once, when it first appears.
    void announceNotification();

    /// The optional once-a-second clock tick.
    void tickTheClock();

    /// Draw the rescue countdown, which outranks everything on the panel.
    /// The countdown shared by both hold gestures. `label` is what the hold
    /// will do, because "RESET" and "SETUP" must not look alike on a panel
    /// somebody is deciding whether to let go of.
    void renderHoldCountdown(Canvas& canvas, const char* label,
                             std::uint64_t remainingMillis,
                             std::uint64_t holdMillis) const;
    void renderNotice(Canvas& canvas, std::uint64_t nowMillis) const;

    /// Draw one setting, label and value, filling the panel.
    void renderSettings(Canvas& canvas) const;

    /// Draw the transient readout shown after − or + while browsing.
    void renderAdjustment(Canvas& canvas) const;

    /// The confirmation beep played when volume changes. A short mid tone:
    /// high enough to carry from a small speaker, short enough that holding
    /// the button does not turn into an alarm.
    static constexpr int kVolumeFeedbackHz = 1000;
    static constexpr int kVolumeFeedbackMillis = 60;

    /// How often the visualiser takes a column, in milliseconds.
    ///
    /// 52 columns at 100 ms is a little over five seconds of history on
    /// screen - slow enough to watch, long enough that a phrase of music has
    /// a shape. Independent of the frame rate on purpose: the trace should
    /// look the same whether the panel is managing 20 FPS or 40.
    static constexpr std::uint64_t kVisualizerSampleMillis = 100;

    /// Loudest reading since the last column was taken, so slowing the trace
    /// down cannot swallow a transient.
    int visualizerPeak_ = 0;
    std::uint64_t lastVisualizerPushMillis_ = 0;

    /// How long the browsing adjustment readout stays up. Long enough to read
    /// after the press that caused it, short enough not to hide the clock.
    static constexpr std::uint64_t kAdjustmentReadoutMillis = 1200;

    input::InputMapper mapper_;
    input::Navigator navigator_;
    input::Rescue rescue_;
    input::SetupHold setupHold_;

    /// Lives on the host rather than inside the app, so it keeps counting
    /// while the carousel is showing something else. A stopwatch that reset
    /// itself every time the clock came round would be a toy.
    apps::Stopwatch stopwatch_;

    /// The reading the glucose app draws. Owned here, like the stopwatch, so a
    /// data source can update it whichever app is on screen. Until one is
    /// wired it holds a demo reading built at startup.
    apps::glucose::Reading glucose_;

    /// One-shot, taken by takeSetupRequest().
    bool setupRequested_ = false;

    /// Whole seconds last shown on the rescue countdown, or -1 when it is not
    /// running. Drives one redraw per second rather than one per frame.
    int lastRescueSecond_ = -1;

    /// The same, for the setup-hold countdown.
    int lastSetupSecond_ = -1;

    /// Whether settings were open on the previous tick, so the tick that closes
    /// them does not immediately bill the carousel for the time spent inside.
    bool wasInSettings_ = false;

    /// What the panel was last told, so brightness is pushed on change rather
    /// than on every tick. 256 is deliberately not a valid byte: it means
    /// nothing has been pushed yet, so the first tick always sends one.
    int appliedBrightness_ = 256;

    /// The parsed timezone and the string it came from, so a rule is parsed
    /// once per change rather than once per rendered frame.
    timezone_::Timezone timezone_;
    std::string timezoneSpec_;
    bool timezoneValid_ = false;

    /// When the on-screen adjustment readout stops being drawn, or 0 when
    /// nothing is showing. Pressing − or + while browsing has to show what
    /// it changed: a brightness step is invisible in daylight and at night
    /// it looks like the whole panel flickered for no reason.
    std::uint64_t adjustmentShownUntilMillis_ = 0;

    /// Which quantity the readout is showing. A bare number answers "something
    /// changed" and not "what", and − / + reach two different things depending
    /// on whether this device has a speaker.
    bool adjustmentIsVolume_ = false;

    /// Sequence of the notification already announced, so a sound plays once
    /// when it appears rather than on every frame it is showing.
    std::uint32_t announcedSequence_ = 0;

    /// Wall-clock second the last tick was played for.
    static constexpr std::int64_t kNoSecond = -1;
    std::int64_t lastTickedSecond_ = kNoSecond;
    render::FrameScheduler scheduler_;

    Framebuffer framebuffer_;
    json::Token sceneTokens_[kSceneTokens];
    scene::Scene scene_;

    std::string parsedAppId_;
    std::uint32_t parsedRevision_ = 0;
    bool sceneReady_ = false;

    BootMode bootMode_ = BootMode::Normal;
    BootRecord bootRecord_;
    bool initialized_ = false;
    bool healthy_ = false;
    bool shutdownRequested_ = false;
    int framesSinceBoot_ = 0;
    std::uint64_t lastTickMillis_ = 0;
    std::uint64_t lastClockMillis_ = 0;
    std::uint32_t persistedIconRevision_ = 0;
    std::uint32_t persistedAppRevision_ = 0;
    /// Display power as of the last rendered frame, so a change made through any
    /// route forces one more redraw. Starts true to match the default setting.
    bool renderedWithPower_ = true;

    /// The frame as it was when the active app last changed, and the clock and
    /// direction of the transition running over it. One extra framebuffer is
    /// 2496 bytes, which is far cheaper than teaching the renderer to draw an
    /// app that is no longer active.
    /// History for the visualiser app. Fed every tick while the microphone
    /// is present, so the trace keeps scrolling whether or not that app is
    /// the one on screen - switching to it mid-sound should show what just
    /// happened, not start from an empty panel.
    apps::Visualizer visualizer_;

    Framebuffer previousFrame_;
    /// The incoming frame, held while it is composited over the outgoing
    /// one. A member rather than a local so the render path allocates
    /// nothing, on the stack or otherwise.
    Framebuffer transitionScratch_;
    std::uint64_t transitionStartMillis_ = 0;
    render::TransitionStyle transitionStyle_ = render::TransitionStyle::None;
    render::TransitionDirection transitionDirection_ = render::TransitionDirection::Forward;
    bool transitionActive_ = false;

    bool splashActive_ = false;
    bool firstRun_ = false;

    /// Empty when there is nothing to say. Held rather than passed per frame
    /// because the detail line scrolls, and scrolling needs a start time.
    std::string noticeTitle_;
    std::string noticeDetail_;
    std::uint64_t noticeStartedMillis_ = 0;

    bool ticking_ = false;
    /// When tick() was first called; the splash and the health timer both
    /// measure from here.
    std::uint64_t firstTickMillis_ = 0;
    /// Built once at boot; rendering it per frame would allocate.
    std::string splashDetail_;

    /// The address alone, for the splash second page lower line.
    std::string splashAddress_;

    // Declared last: its context holds pointers to the members above, which must
    // already be constructed when it is built.
    api::ApiServer apiServer_;
    web::StaticFiles staticFiles_;
    mqtt::MqttService mqtt_;
    net::ScriptFetcher fetcher_;
};

}  // namespace host
}  // namespace stipple
