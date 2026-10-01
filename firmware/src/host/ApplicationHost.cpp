// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/host/ApplicationHost.h"

#include "stipple/api/BasicAuth.h"

#include <cstdio>

#include "stipple/apps/BatteryApp.h"
#include "stipple/apps/GlucoseApp.h"
#include "stipple/apps/StopwatchApp.h"
#include "stipple/render/Overlay.h"
#include "stipple/apps/Unavailable.h"
#include "stipple/apps/VisualizerApp.h"

#include "stipple/api/JsonWriter.h"
#include "stipple/core/Version.h"
#include "stipple/demo/TestPattern.h"
#include "stipple/graphics/Canvas.h"
#include "stipple/text/Text.h"

namespace stipple {
namespace host {

const char* bootModeName(BootMode mode) noexcept {
    switch (mode) {
        case BootMode::Normal: return "normal";
        case BootMode::SafeMode: return "safe";
    }
    return "unknown";
}

ApplicationHost::ApplicationHost(platform::IPlatformServices& platform, HostConfig config)
    : platform_(platform),
      config_(std::move(config)),
      configStore_(platform.storage()),
      carousel_(registry_),
      scene_(sceneTokens_, kSceneTokens),
      apiServer_(makeContext(), config_.api) {}

api::ApiContext ApplicationHost::makeContext() noexcept {
    api::ApiContext context;
    context.apps = &registry_;
    context.carousel = &carousel_;
    context.notifications = &notifications_;
    context.icons = &icons_;
    context.config = &settings_;
    context.configStore = &configStore_;
    context.firstRun = &firstRun_;
    context.platform = &platform_;
    context.logger = &logger_;
    context.frame = &framebuffer_;
    context.input = this;
    context.scheduler = &scheduler_;
    context.glucose = &glucoseSource_;
    context.glucoseAlarm = &glucoseAlarm_;
    return context;
}

// --- boot state --------------------------------------------------------------

BootRecord ApplicationHost::readBootRecord() {
    BootRecord record;

    std::string stored;
    if (!platform_.storage().read(kBootStateKey, stored)) {
        return record;  // first ever boot
    }

    json::Token tokens[16];
    json::Document document(tokens, 16);
    if (document.parse(stored) != json::Error::None) {
        // Unreadable boot state is itself suspicious, but it must not be what
        // stops the device starting.
        logger_.warn(platform_.clock().monotonicMillis(),
                     "boot record unreadable; treating as first boot");
        return record;
    }

    const json::Value root = document.root();
    const std::int64_t failures = root["consecutiveFailures"].toInt(0);
    record.consecutiveFailures =
        failures < 0 ? 0u : static_cast<std::uint32_t>(failures > 100 ? 100 : failures);
    record.lastBootCompleted = root["lastBootCompleted"].toBool(true);
    return record;
}

void ApplicationHost::writeBootRecord(const BootRecord& record) {
    api::JsonWriter writer;
    writer.beginObject()
        .member("consecutiveFailures", static_cast<std::int64_t>(record.consecutiveFailures))
        .member("lastBootCompleted", record.lastBootCompleted)
        .endObject();

    if (!platform_.storage().write(kBootStateKey, writer.str())) {
        // Losing the marker costs the anti-brick guarantee but not this boot.
        logger_.error(lastTickMillis_, "could not persist boot record");
    }
}

void ApplicationHost::markHealthy() {
    healthy_ = true;

    BootRecord record;
    record.consecutiveFailures = 0;
    record.lastBootCompleted = true;
    writeBootRecord(record);
    bootRecord_ = record;

    logger_.info(lastTickMillis_, "boot healthy");
}

// --- startup -----------------------------------------------------------------

bool ApplicationHost::initialize() {
    // Startup used to log with a literal 0, so every boot line rendered as
    // 00:00:00 and sorted before everything else. The clock is available the
    // whole time; there was never a reason not to ask it.
    const std::uint64_t startedAt = platform_.clock().monotonicMillis();

    // 1. Logging first, so everything that follows can be recorded.
    logger_.info(startedAt, "STIPPLE starting");

    // 2. Boot state, before anything that could crash.
    BootRecord record = readBootRecord();
    if (!record.lastBootCompleted) {
        ++record.consecutiveFailures;
        logger_.warn(startedAt, "previous boot did not complete");
    }

    bootMode_ = record.consecutiveFailures >= static_cast<std::uint32_t>(config_.safeModeThreshold)
                    ? BootMode::SafeMode
                    : BootMode::Normal;
    if (bootMode_ == BootMode::SafeMode) {
        logger_.error(startedAt, "repeated boot failures; starting in safe mode");
    }

    record.lastBootCompleted = false;
    writeBootRecord(record);
    bootRecord_ = record;

    scene_.setIconStore(&icons_);

    // 3. Display. The panel's own floor beats any configured target rate.
    scheduler_.setMinimumInterval(platform_.display().minimumFrameIntervalMillis());

    // 4. Settings. Safe mode deliberately ignores stored configuration, since a
    //    bad value in it is one of the things that could have caused the
    //    failures that got us here.
    if (bootMode_ == BootMode::SafeMode) {
        settings_ = config::Config{};
        logger_.warn(startedAt, "safe mode: using default settings");
    } else {
        const config::LoadReport report = configStore_.load(settings_);
        logger_.info(startedAt, config::describe(report.status));

        // Nothing stored means nobody has ever set this device up. Corrupt
        // storage deliberately does not count: that device *was* configured,
        // and telling its owner it is brand new would be both wrong and the
        // least helpful thing to say while they are trying to work out what
        // happened to their settings.
        firstRun_ = report.status == config::LoadStatus::DefaultsMissing;
        if (firstRun_) {
            logger_.info(startedAt, "first run: nothing configured yet");
        }
    }
    applyBrightness();
    if (platform_.audio() != nullptr) {
        platform_.audio()->setVolume(config::volumeToByte(settings_.audio.volumePercent));
    }
    sharedSpeaker_.attach(platform_.audio());
    glucoseAlarm_.configure(settings_.glucose.alarms);

    applyCarouselSettings();

    // 5. Apps and stored assets.
    if (bootMode_ == BootMode::Normal) {
        installBuiltins();
        applyStoredAppOrder();
        loadIcons();
        // A glucose display boots onto glucose, not onto the clock. After the
        // stored order so a disabled app fails the pin cleanly.
        glucoseSource_.configure(settings_.glucose.url, settings_.glucose.apiSecretSha1,
                                 settings_.glucose.pollSeconds);
        // Here as well as in setScriptRunner: a glucose display whose source
        // only got a client when a script runner was installed would be an
        // alarm that depends on the scripting engine.
        glucoseSource_.setClient(platform_.httpClient());
        if (glucoseHoldWanted()) {
            carousel_.pin(kGlucoseAppId, startedAt);
        }
    } else {
        logger_.warn(startedAt, "safe mode: no apps or icons loaded");
    }
    persistedIconRevision_ = icons_.revision();

    // 6. Capabilities this build does not have. Logged rather than silently
    //    absent, so a device that cannot be reached says why.
    if (platform_.network() == nullptr) {
        logger_.info(startedAt, "no network interface on this platform");
    }
    if (platform_.httpServer() == nullptr) {
        logger_.info(startedAt, "no HTTP transport; API is reachable in-process only");
    }
    mqtt::ServiceContext mqttContext;
    mqttContext.client = platform_.mqtt();
    mqttContext.api = &apiServer_;
    mqttContext.settings = &settings_;
    mqttContext.logger = &logger_;
    mqtt_.setContext(mqttContext);
    if (bootMode_ == BootMode::Normal) {
        mqtt_.configure(startedAt);
    } else {
        // Safe mode stays off the network entirely. Whatever put the device here
        // might be reachable from a broker, and a boot loop that republishes
        // retained state each time is worse than a quiet one.
        logger_.warn(startedAt, "safe mode: MQTT not started");
    }

    // Volume is only offered where something can make a sound. On a 52x16
    // panel the honest way to show an absent capability is to leave the control
    // out, not to list one that does nothing (ADR 0013) - which is exactly what
    // the old default bindings did by putting volume on the − / + taps of a
    // device with no speaker.
    navigator_.setAvailable(input::SettingSlot::Volume, platform_.audio() != nullptr);
    if (platform_.audio() == nullptr) {
        logger_.info(startedAt, "no audio output; volume is not offered in settings");
        // Said louder than the line above: on a glucose display this is the
        // difference between an alarm and a number nobody hears.
        logger_.warn(startedAt, "no speaker: glucose alarms cannot sound");
    }

    splashDetail_ = apps::splashDetail(kVersion, platform_.network());
    splashAddress_ = apps::splashAddress(platform_.network());
    splashActive_ = config_.splashMillis > 0;

    initialized_ = true;
    logger_.info(startedAt, "startup complete");
    return true;
}

void ApplicationHost::installBuiltins() {
    if (!config_.installClockApp) {
        return;
    }
    app::App clock;
    clock.id = std::string(kClockAppId);
    clock.name = "Clock";
    clock.source = app::AppSource::System;
    clock.builtin = app::Builtin::Clock;
    clock.durationSeconds = 0;  // uses the carousel default
    registry_.put(std::move(clock));

    // Second in the rotation, immediately after the clock.
    //
    // Order here is insertion order, and it is the default a device ships
    // with rather than a rule - the app manager owns arrangement and the user
    // can move it. Placed here because a stopwatch is the thing people reach
    // for on a clock that is already in the room, and because it needs no
    // hardware at all: unlike the battery and visualiser cards below, there
    // is no platform to check first.
    app::App stopwatch;
    stopwatch.id = std::string(kStopwatchAppId);
    stopwatch.name = "Stopwatch";
    stopwatch.source = app::AppSource::System;
    stopwatch.builtin = app::Builtin::Stopwatch;
    stopwatch.durationSeconds = 0;
    registry_.put(std::move(stopwatch));

    // Battery is installed only where the platform can actually report one.
    // Registering it unconditionally would put a permanent "NO BATT" card in
    // the rotation of every mains-only panel, which is the carousel equivalent
    // of a switch that does nothing.
    // Same rule as the battery app: installed only where the hardware can
    // actually feed it. A visualiser permanently showing NO MIC is a card in
    // the rotation that exists to apologise.
    if (platform_.microphone() != nullptr) {
        app::App visualizer;
        visualizer.id = std::string(kVisualizerAppId);
        visualizer.name = "Visualizer";
        visualizer.source = app::AppSource::System;
        visualizer.builtin = app::Builtin::Visualizer;
        visualizer.durationSeconds = 0;
        registry_.put(std::move(visualizer));
    }

    if (platform_.power() != nullptr) {
        app::App battery;
        battery.id = std::string(kBatteryAppId);
        battery.name = "Battery";
        battery.source = app::AppSource::System;
        battery.builtin = app::Builtin::Battery;
        battery.durationSeconds = 0;
        registry_.put(std::move(battery));
    }

    // Needs no hardware to check for. With no source configured it shows its
    // no-data face, which is the honest thing for a glucose display to say.
    app::App glucose;
    glucose.id = std::string(kGlucoseAppId);
    glucose.name = "Glucose";
    glucose.source = app::AppSource::System;
    glucose.builtin = app::Builtin::Glucose;
    glucose.durationSeconds = 0;
    registry_.put(std::move(glucose));
}

void ApplicationHost::loadIcons() {
    std::string blob;
    if (!platform_.storage().read(kIconStateKey, blob)) {
        return;  // nothing stored yet
    }

    if (!icons_.deserialize(blob)) {
        // Corrupt icon data must not stop the device starting; it just means no
        // icons. Dropping the key avoids re-reading the same broken blob every
        // boot and keeps the failure from looking intermittent.
        logger_.warn(platform_.clock().monotonicMillis(),
                     "stored icons unreadable; discarding them");
        platform_.storage().remove(kIconStateKey);
        return;
    }
    logger_.info(platform_.clock().monotonicMillis(), "icons loaded");
}

void ApplicationHost::publishScriptEnvironment() {
    if (scripts_ == nullptr) {
        return;
    }

    script::ScriptEnvironment environment;

    const platform::ISystemClock& clock = platform_.clock();
    environment.monotonicMillis = clock.monotonicMillis();
    environment.timeKnown = clock.wallClockValid();
    if (environment.timeKnown) {
        const std::int64_t local = clock.unixSeconds() + clock.utcOffsetSeconds();

        // Floor division rather than truncation. Before 1970 a truncating
        // divide lands on the wrong day - which nobody will ever see on this
        // device, and which is still not a reason to write the subtly wrong
        // one.
        constexpr std::int64_t kSecondsPerDay = 86400;
        std::int64_t secondsOfDay = local % kSecondsPerDay;
        if (secondsOfDay < 0) {
            secondsOfDay += kSecondsPerDay;
        }
        environment.hour = static_cast<int>(secondsOfDay / 3600);
        environment.minute = static_cast<int>((secondsOfDay % 3600) / 60);
        environment.second = static_cast<int>(secondsOfDay % 60);

        // civilFromUnix works in UTC, so it gets the already-offset time -
        // which is exactly the local civil date.
        const timezone_::CivilDate date = timezone_::civilFromUnix(local);
        environment.year = date.year;
        environment.month = date.month;
        environment.day = date.day;
        environment.weekday = date.weekday;
    }

    if (platform::IPowerSource* power = platform_.power()) {
        const platform::BatteryStatus battery = power->battery();
        environment.batteryKnown = battery.known;
        environment.batteryPercent = battery.percent;
        environment.charging = battery.charging;
    }

    scripts_->setEnvironment(environment);
}

void ApplicationHost::setScriptRunner(script::IScriptRunner* runner) {
    scripts_ = runner;

    // The API gets the same one. Two places holding different answers to "can
    // this device run scripts" is the kind of drift that shows up as an app on
    // the panel the web UI insists does not exist.
    apiServer_.setScriptRunner(runner);

    if (runner == nullptr) {
        return;
    }

    // Load here rather than in initialize(). The runner is owned outside the
    // core and installed after the host is up, so at initialize() time there
    // is nothing to load into - and a script library that only appeared after
    // the next reboot would look exactly like one that had not saved.
    // The speaker, if this device has one. Null is a supported answer and
    // the builtins report it rather than pretending to play.
    // Through the shared speaker, so a script's tone cannot cut off an alarm.
    runner->setAudio(platform_.audio() != nullptr ? &sharedSpeaker_ : nullptr);

    // And the broker. Always handed over, even with MQTT switched off: the
    // gateway reports its own state, so a script asking mqtt_known() gets a
    // straight answer either way, and turning MQTT on later needs no second
    // call from here.
    runner->setMqtt(&mqtt_.scripts());

    // And the network. The fetcher reports its own availability, so a
    // platform with no HTTP client is a script being told it cannot fetch
    // rather than requests queueing for a socket that will never exist.
    fetcher_.setClient(platform_.httpClient());
    glucoseSource_.setClient(platform_.httpClient());
    runner->setHttp(&fetcher_);

    // And the microphone, for the visualisers. Null is a device that cannot
    // hear, which the builtins report rather than reading as silence.
    runner->setMicrophone(platform_.microphone());

    loadScripts();
    persistedScriptRevision_ = runner->revision();
}

void ApplicationHost::loadScripts() {
    if (scripts_ == nullptr || bootMode_ != BootMode::Normal) {
        return;  // safe mode deliberately runs nothing that arrived over the network
    }

    std::string blob;
    if (!platform_.storage().read(kScriptStateKey, blob)) {
        return;  // nothing stored yet
    }

    if (!scripts_->deserialize(blob)) {
        // Same reasoning as icons: corrupt data must not stop the device
        // starting, and dropping the key keeps one bad write from looking like
        // an intermittent fault every boot after.
        logger_.warn(platform_.clock().monotonicMillis(),
                     "stored scripts unreadable; discarding them");
        platform_.storage().remove(kScriptStateKey);
        return;
    }
    // Every restored script gets its app back.
    //
    // Without this, scripts silently stopped appearing after a reboot. The
    // API creates a script's app when the script is written, but that only
    // covers the write: on the next boot the scripts came back from their own
    // blob while the apps came from the stored carousel order, which had
    // never been told about them. The script listed as perfectly fine and
    // could not reach the panel.
    //
    // Found on hardware, not in a test - the device had an aquarium script
    // with nothing to show it.
    //
    // Only where the app is missing. An app restored from the stored order
    // already carries the position, duration and enabled flag somebody chose,
    // and recreating it would throw all three away every time the device
    // started.
    int restored = 0;
    for (int i = 0; i < scripts_->count(); ++i) {
        const script::Script* entry = scripts_->at(i);
        if (entry == nullptr || registry_.find(entry->id) != nullptr) {
            continue;
        }
        app::App app;
        app.id = entry->id;
        app.name = entry->name;
        app.builtin = app::Builtin::Script;
        app.source = app::AppSource::Local;
        if (const std::uint32_t wanted = scripts_->durationMillis(entry->id); wanted > 0) {
            app.durationSeconds = static_cast<int>((wanted + 999u) / 1000u);
        }
        registry_.put(std::move(app));
        ++restored;
    }

    logger_.info(platform_.clock().monotonicMillis(), "scripts loaded");

    if (restored > 0) {
        // And the stored arrangement applied again, now that those apps
        // exist.
        //
        // initialize() already did this once, before the scripts were loaded,
        // so every script's stored position, duration and enabled flag was
        // skipped as "an app that no longer exists". Recreating the app
        // without this gives it back at the end of the carousel, switched on,
        // at the default duration - undoing a choice somebody made on purpose
        // every time the device started.
        //
        // Safe to repeat: it walks the stored order and ignores what it
        // cannot find, so the second pass only fills in what the first could
        // not.
        applyStoredAppOrder();
        logger_.info(platform_.clock().monotonicMillis(), "restored apps for scripts");
    }
}

void ApplicationHost::persistScriptsIfChanged() {
    if (scripts_ == nullptr || scripts_->revision() == persistedScriptRevision_) {
        return;
    }
    persistedScriptRevision_ = scripts_->revision();

    if (scripts_->count() == 0) {
        platform_.storage().remove(kScriptStateKey);
        return;
    }
    if (!platform_.storage().write(kScriptStateKey, scripts_->serialize())) {
        logger_.error(lastTickMillis_, "could not persist scripts");
    }
}

void ApplicationHost::persistIconsIfChanged() {
    if (icons_.revision() == persistedIconRevision_) {
        return;
    }
    persistedIconRevision_ = icons_.revision();

    if (icons_.count() == 0) {
        platform_.storage().remove(kIconStateKey);
        return;
    }
    if (!platform_.storage().write(kIconStateKey, icons_.serialize())) {
        logger_.error(lastTickMillis_, "could not persist icons");
    }
}

void ApplicationHost::shutdown() {
    if (shutdownRequested_) {
        return;
    }
    shutdownRequested_ = true;

    // Before the boot record, so availability flips even if writing that fails.
    mqtt_.shutdown();

    // Only record a clean shutdown if the boot actually succeeded; otherwise the
    // failure counter must survive to trigger safe mode next time.
    if (healthy_) {
        BootRecord record;
        record.consecutiveFailures = 0;
        record.lastBootCompleted = true;
        writeBootRecord(record);
    }
    logger_.info(lastTickMillis_, "shutdown");
}

// --- input -------------------------------------------------------------------

void ApplicationHost::dismissSplash() noexcept {
    if (splashActive_) {
        splashActive_ = false;
        scheduler_.invalidate();
    }
}

void ApplicationHost::handleInput(const platform::InputEvent& event) {
    // Before everything else, deliberately (ADR 0018). The rescue gesture has
    // to work while a notification is up, while settings are open, and on a
    // device whose network or password is the thing that is broken. A way back
    // in that can be blocked by whatever is on screen is not a way back in.
    rescue_.handle(event);

    // Same reasoning, same position in the order: a hold that only works on
    // the carousel is one nobody can rely on.
    setupHold_.handle(event);

    // The knob press snoozes a sounding alarm - wherever it is pressed, and
    // ahead of the splash, settings and every binding, because "make it stop"
    // must not depend on what is on screen or how the buttons are mapped. Taken
    // on the Down so it answers at once; the matching Up is swallowed, so the
    // press does nothing else and a held knob never opens settings.
    if (event.source == platform::RawInput::RotaryPress) {
        if (event.phase == platform::ButtonPhase::Down && glucoseAlarm_.sounding()) {
            snoozeGlucoseAlarm();
            swallowKnobRelease_ = true;
            mapper_.reset();
            return;
        }
        if (event.phase == platform::ButtonPhase::Up && swallowKnobRelease_) {
            swallowKnobRelease_ = false;
            return;
        }
    }

    // Any interaction means the user is looking at the device and wants to get
    // on with it. The press is consumed rather than also performing its normal
    // action: someone tapping a button to skip the splash does not expect to
    // silently pause the carousel at the same time.
    if (splashActive_) {
        dismissSplash();
        mapper_.reset();  // no half-finished press survives the transition
        return;
    }

    // Someone pressing a button has already decided; making them watch the
    // rest of an animation is the interface arguing (DESIGN.md section 6).
    transitionActive_ = false;

    input::ActionEvent action;
    if (!mapper_.handle(event, action)) {
        return;
    }

    // A script that asked for the controls gets them before anything else
    // looks at the action (ADR 0024). Before, because the whole point is that
    // the knob moves a paddle instead of the carousel - checking afterwards
    // would mean the app had already changed.
    if (offerToScript(action.action)) {
        scheduler_.invalidate();
        return;
    }

    switch (action.action) {
        // action.repeat is deliberately ignored for navigation.
        //
        // InputMapper accelerates detents that arrive within 120 ms, up to 5x,
        // which is right for a continuous value and wrong for a short list. On
        // a device with three apps, any ordinary turn of the knob jumped two to
        // five of them and landed somewhere that looked arbitrary - the carousel
        // "weirdly moving between apps".
        //
        // Nobody spins a knob to skip apps; they turn it to look at the next
        // one. One detent, one app, however fast the wrist. Acceleration stays
        // where it earns its place, on brightness and volume below.
        case input::Action::AppNext:
            // The knob means "move between things" in both modes; only the
            // things differ (ADR 0017). Inside settings that is the cursor.
            if (navigator_.inSettings()) {
                navigator_.moveCursor(1, lastTickMillis_);
                break;
            }
            // While the glucose app holds the screen, the things are its
            // faces. Still one detent, one thing - and never carousel_.next(),
            // which would drop the pin. The middle button is the way out.
            if (carousel_.isPinned() && carousel_.pinnedId() == kGlucoseAppId) {
                stepGlucoseFace(1);
                break;
            }
            transitionDirection_ = render::TransitionDirection::Forward;
            carousel_.next(lastTickMillis_);
            break;
        case input::Action::AppPrevious:
            if (navigator_.inSettings()) {
                navigator_.moveCursor(-1, lastTickMillis_);
                break;
            }
            if (carousel_.isPinned() && carousel_.pinnedId() == kGlucoseAppId) {
                stepGlucoseFace(-1);
                break;
            }
            transitionDirection_ = render::TransitionDirection::Backward;
            carousel_.previous(lastTickMillis_);
            break;
        case input::Action::AppAction:
            if (navigator_.inSettings()) {
                activateCurrentSetting();
                navigator_.noteActivity(lastTickMillis_);
                break;
            }
            // The stopwatch takes the press instead of pausing the carousel.
            //
            // Start, stop and reset are what somebody looking at a stopwatch
            // wants from the only control an app is given, and pausing the
            // rotation is not. The rotation is handled anyway: it is held
            // while the stopwatch runs, below, so starting it does not send
            // the thing you are timing off the screen.
            if (const app::App* active = carousel_.active();
                active != nullptr && active->builtin == app::Builtin::Stopwatch) {
                stopwatch_.press(lastTickMillis_);
                scheduler_.invalidate();
                break;
            }
            // A script gets the press for the same reason the stopwatch does:
            // this is the only control an app is given, and for an app that
            // wants one, pausing the carousel is not what it is for.
            //
            // Only if the script actually has an on_button. One that does not
            // must let the press fall through - a script that silently
            // swallowed the only button would be an app you could not pause
            // and would look like a device that had stopped responding.
            //
            // The knob is deliberately not offered. It is how somebody moves
            // between apps, and a script that took it would be a script you
            // could not leave.
            //
            // "select" rather than "action", because that is the name the
            // scripts people have already written test for. The point of
            // matching the documented interface is that their scripts run
            // here unchanged, and a different word for the only button would
            // undo most of that for the sake of a nicer noun.
            if (scripts_ != nullptr) {
                if (const app::App* active = carousel_.active();
                    active != nullptr && active->builtin == app::Builtin::Script &&
                    scripts_->button(active->id, "select")) {
                    scheduler_.invalidate();
                    break;
                }
            }
            carousel_.setPaused(!carousel_.paused());
            break;
        case input::Action::NotificationDismiss:
            if (!notifications_.dismissActive(lastTickMillis_)) {
                carousel_.setPaused(!carousel_.paused());
            }
            break;
        case input::Action::Back:
            // Always backwards, wherever it arrives from. Leaving settings
            // first, then dismissing a notification, then returning to the
            // clock - each step is one the user can see having happened, which
            // is what stops a "back" button feeling like a coin toss.
            if (navigator_.inSettings()) {
                navigator_.exitSettings();
                break;
            }
            if (notifications_.dismissActive(lastTickMillis_)) {
                break;
            }
            if (carousel_.activate(kClockAppId, lastTickMillis_)) {
                transitionDirection_ = render::TransitionDirection::Backward;
            } else if (carousel_.isPinned() && carousel_.pinnedId() == kGlucoseAppId) {
                // No clock to go back to (disabled, or not installed). A held
                // glucose app with no way out would be the trap ADR 0017
                // forbids, so "back" still leaves - to whatever is next.
                carousel_.unpin();
                transitionDirection_ = render::TransitionDirection::Backward;
                carousel_.next(lastTickMillis_);
            }
            break;
        case input::Action::SettingsToggle:
            navigator_.toggleSettings(lastTickMillis_);
            break;
        case input::Action::AdjustUp:
        case input::Action::AdjustDown: {
            const int direction = action.action == input::Action::AdjustUp ? 1 : -1;
            const int steps = direction * action.repeat;
            if (navigator_.inSettings()) {
                adjustCurrentSetting(steps);
                navigator_.noteActivity(lastTickMillis_);
                break;
            }
            // Browsing: volume where there is a speaker, brightness where
            // there is not.
            //
            // Not a second meaning for the control - it still adjusts "the
            // thing" - but the thing at the top level depends on what the
            // device can actually do. On hardware with audio, volume is what
            // people reach for; on hardware without, falling through to
            // brightness keeps the buttons useful rather than letting them go
            // dead, which is the defect this whole model exists to avoid.
            if (!adjustVolume(steps)) {
                adjustBrightness(steps);
            }
            // Shown on screen because a brightness step is invisible in
            // daylight and at night reads as the panel having glitched. A
            // control with no feedback is indistinguishable from a broken one,
            // which is what put volume on these buttons for so long without
            // anyone noticing it did nothing.
            adjustmentShownUntilMillis_ = lastTickMillis_ + kAdjustmentReadoutMillis;
            adjustmentIsVolume_ = platform_.audio() != nullptr;
            adjustmentIsFace_ = false;
            adjustmentIsSnooze_ = false;
            break;
        }
        case input::Action::BrightnessUp:
        case input::Action::BrightnessDown:
            // Named rather than relative, so an API or MQTT caller with no
            // on-device context still gets exactly what it asked for. Also
            // what holding − or + does, which is why it shows the readout: a
            // brightness step is invisible in daylight and at night reads as
            // the panel having glitched.
            adjustBrightness((action.action == input::Action::BrightnessUp ? 1 : -1) *
                             action.repeat);
            adjustmentShownUntilMillis_ = lastTickMillis_ + kAdjustmentReadoutMillis;
            adjustmentIsVolume_ = false;
            adjustmentIsFace_ = false;
            adjustmentIsSnooze_ = false;
            break;
        case input::Action::VolumeUp:
        case input::Action::VolumeDown:
            adjustVolume((action.action == input::Action::VolumeUp ? 1 : -1) * action.repeat);
            break;
        case input::Action::None:
            break;
    }

    // Automations can react to the hardware even when the action itself is
    // local (§20). Published after handling, so a subscriber never sees an
    // event the device has not already acted on.
    mqtt_.publishButton(input::actionName(action.action), action.repeat, action.longPress);

    scheduler_.invalidate();
}

void ApplicationHost::pumpInput(std::uint64_t nowMillis) {
    (void)nowMillis;
    platform::InputEvent event;
    while (platform_.input().poll(event)) {
        handleInput(event);
    }
}

// --- adjustment ---------------------------------------------------------------

void ApplicationHost::adjustBrightness(int steps) {
    const int step = mapper_.config().brightnessStep;
    int level = static_cast<int>(settings_.display.brightness) + step * steps;
    level = level < 0 ? 0 : (level > 255 ? 255 : level);
    settings_.display.brightness = static_cast<std::uint8_t>(level);
    applyBrightness();

    // Turning the panel up is also the obvious way to ask for it back after
    // switching it off, and leaving it dark would look like the button had
    // failed.
    if (level > 0) {
        settings_.display.power = true;
    }
}

bool ApplicationHost::adjustVolume(int steps) {
    // Refused rather than faked when the platform has no speaker: an absent
    // capability is reported at boot, not papered over here (ADR 0013). The
    // return value is what lets settings hide the control entirely instead of
    // offering one that does nothing.
    if (platform_.audio() == nullptr) {
        return false;
    }
    // Percent, because that is how a volume control reads to a person,
    // converted once at the edge where the hardware wants 0-255.
    const int step = mapper_.config().volumeStepPercent;
    int percent = static_cast<int>(settings_.audio.volumePercent) + step * steps;
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    settings_.audio.volumePercent = static_cast<std::uint8_t>(percent);
    platform_.audio()->setVolume(config::volumeToByte(settings_.audio.volumePercent));

    // A short beep at the new level.
    //
    // Setting a volume you cannot hear is guesswork, and on a panel that shows
    // one number at a time the number is the only feedback there would be.
    // Every device with a volume control does this, for the same reason.
    //
    // Skipped at zero: a confirmation beep for "silence" is a contradiction,
    // and it is the one setting where the absence of sound is the feedback.
    if (percent > 0) {
        sharedSpeaker_.playTone(kVolumeFeedbackHz, kVolumeFeedbackMillis);
    }
    return true;
}

void ApplicationHost::adjustCurrentSetting(int steps) {
    if (steps == 0) {
        return;
    }
    switch (navigator_.current()) {
        case input::SettingSlot::Brightness:
            adjustBrightness(steps);
            break;
        case input::SettingSlot::Overlay: {
            const int count = render::kOverlayCount;
            int index = 0;
            const render::Overlay active = render::overlayFromName(settings_.display.overlay);
            for (int i = 0; i < count; ++i) {
                if (render::overlayAt(i) == active) {
                    index = i;
                    break;
                }
            }
            // Wraps, because a list of five on a panel that shows one at a time
            // should not have ends a user can get stuck against.
            index = ((index + steps) % count + count) % count;
            settings_.display.overlay = render::overlayName(render::overlayAt(index));
            break;
        }
        case input::SettingSlot::Volume:
            adjustVolume(steps);
            break;
        case input::SettingSlot::Count:
            break;
    }
}

bool ApplicationHost::offerToScript(input::Action action) {
    if (scripts_ == nullptr || navigator_.inSettings() || splashActive_) {
        return false;
    }

    const app::App* active = carousel_.active();
    if (active == nullptr || active->builtin != app::Builtin::Script) {
        return false;
    }
    if (scripts_->inputMode(active->id) != script::InputMode::Exclusive) {
        return false;
    }

    // The names /api/v1/input already uses, so a press from a browser, a
    // broker and a thumb are indistinguishable to the script - which is what
    // makes a phone a usable controller without the firmware knowing.
    //
    // "select" rather than "press" because that is the name every script
    // written before this tests for, and running those unchanged is worth
    // more than a tidier noun.
    const char* name = nullptr;
    switch (action) {
        case input::Action::AppPrevious:  name = "left";   break;
        case input::Action::AppNext:      name = "right";  break;
        case input::Action::AppAction:    name = "select"; break;
        case input::Action::AdjustDown:   name = "minus";  break;
        case input::Action::AdjustUp:     name = "plus";   break;

        // Everything else stays where it was, and two of them deliberately:
        //
        //   Back / SettingsToggle - the middle button and a held knob. These
        //   are the way out, and a script that could take them would be a
        //   script you could not leave. That was the whole objection to
        //   offering the knob at all, and reserving these answers it.
        //
        //   BrightnessUp / BrightnessDown - holding - or +. A game has no use
        //   for them and a person squinting at a dim panel does.
        default:
            return false;
    }

    // action.repeat is ignored, for the same reason navigation ignores it: a
    // fast wrist accelerates detents up to 5x, which is right for brightness
    // and wrong for a paddle. One detent, one event.
    //
    // False when the script has no on_button at all, and the action then does
    // its ordinary job - a script that declared @input and forgot to handle it
    // would otherwise be an app with five dead controls and no way to tell.
    return scripts_->button(active->id, name);
}

void ApplicationHost::activateCurrentSetting() {
    // Nothing yet, and deliberately nothing.
    //
    // The knob press "acts on the thing". Every setting reachable from the
    // panel is a value that − / + already adjust, so there is nothing here to
    // act on - and inventing something for the press to do, a reset or a jump
    // to a default, would put a hidden destructive gesture on the control
    // people press most. This exists for the first setting that is genuinely a
    // toggle.
}

void ApplicationHost::applyCarouselSettings() {
    // Re-applied whenever it differs rather than copied once at startup.
    //
    // It was set in initialize() and nowhere else, so changing the app
    // duration over the API updated the stored setting and did nothing at all
    // until the next restart - which reads as the setting being ignored,
    // because from outside that is exactly what it was.
    //
    // Compared rather than assigned blindly: this runs every tick, and a
    // carousel told its configuration had changed would be entitled to act on
    // that. Today it would not, but a free "nothing changed" check is cheaper
    // than depending on it never starting to.
    if (carousel_.config().defaultDurationSeconds == settings_.apps.defaultDurationSeconds &&
        carousel_.config().autoAdvance == settings_.apps.autoAdvance) {
        return;
    }
    app::CarouselConfig carousel;
    carousel.defaultDurationSeconds = settings_.apps.defaultDurationSeconds;
    carousel.autoAdvance = settings_.apps.autoAdvance;
    carousel_.setConfig(carousel);
}

// --- rescue -------------------------------------------------------------------

void ApplicationHost::clearHotspotRequest() {
    if (!settings_.network.hotspotRequested) {
        return;
    }
    settings_.network.hotspotRequested = false;
    if (!configStore_.save(settings_)) {
        // Worth saying. If this does not persist the device is fine now and
        // back in setup mode after the next reboot, which is the failure
        // this function exists to end.
        logger_.error(lastTickMillis_, "could not clear setup mode");
        return;
    }
    logger_.info(lastTickMillis_, "on a network again; setup mode cleared");
}

void ApplicationHost::performSetupRequest() {
    // Nothing is cleared and nothing is saved. A hotspot asked for by
    // somebody standing at the device should not outlive the reboot they do
    // next - and the persisted flag already has a narrower meaning that this
    // must not quietly widen.
    setupRequested_ = true;
    logger_.info(lastTickMillis_, "setup mode requested from the knob");
    scheduler_.invalidate();
}

void ApplicationHost::performRescue() {
    // Deliberately narrow. This clears the way back in and nothing else:
    // somebody locked out of a clock wants their apps and settings to still be
    // there afterwards, and a rescue that costs a week of an integration's work
    // is one people avoid using until it is too late.
    settings_.web.password.clear();
    settings_.web.username.clear();
    settings_.network.hotspotRequested = true;

    if (!configStore_.save(settings_)) {
        // Said out loud rather than swallowed. If this did not persist, the
        // device is open now and locked again after the next reboot - which is
        // the worst of both and the one outcome nobody could diagnose.
        logger_.error(lastTickMillis_, "rescue applied but could not be saved");
    } else {
        logger_.warn(lastTickMillis_, "rescue: access password cleared");
    }

    scheduler_.invalidate();
}

// --- overnight dimming --------------------------------------------------------

bool ApplicationHost::nightModeActive() const {
    const config::NightSettings& night = settings_.display.night;
    if (!night.enabled) {
        return false;
    }

    const platform::ISystemClock& clock = platform_.clock();
    if (!clock.wallClockValid()) {
        // Without a date there is no local time, and dimming a panel because
        // NTP has not answered yet would look exactly like a fault.
        return false;
    }

    const std::int64_t local = clock.unixSeconds() + currentUtcOffsetSeconds();
    // Floor rather than truncate: a local time west of UTC before the epoch is
    // negative, and so is a device whose clock has not been set properly.
    const std::int64_t dayStart = (local >= 0 ? local / 86400 : (local - 86399) / 86400) * 86400;
    const int minutes = static_cast<int>((local - dayStart) / 60);

    if (night.startMinutes == night.endMinutes) {
        return false;  // a window of no length is not a window
    }
    if (night.startMinutes < night.endMinutes) {
        return minutes >= night.startMinutes && minutes < night.endMinutes;
    }
    // Wrapping midnight, which is the normal case for a night: the window is
    // everything outside the two times rather than between them.
    return minutes >= night.startMinutes || minutes < night.endMinutes;
}

void ApplicationHost::applyBrightness() {
    // The night value overrides the setting without overwriting it, so the
    // morning gets the panel back exactly as the user left it rather than at
    // whatever it was dimmed to. Which is also why brightness is pushed from
    // here rather than written straight to the display when the setting
    // changes - there are now two things that decide it.
    std::uint8_t wanted = nightModeActive() ? settings_.display.night.brightness
                                            : settings_.display.brightness;
    if (glucoseAlarm_.sounding() && wanted < kAlarmBrightnessFloor) {
        wanted = kAlarmBrightnessFloor;
    }
    if (wanted == appliedBrightness_) {
        return;
    }
    appliedBrightness_ = wanted;
    platform_.display().setBrightness(wanted);
}

// --- app order ---------------------------------------------------------------

void ApplicationHost::applyStoredAppOrder() {
    const std::vector<config::AppPreference>& stored = settings_.apps.order;
    if (stored.empty()) {
        return;
    }

    // Walked in stored order, moving each app it names to the front of the
    // remainder. Apps the store does not mention end up after the ones it does,
    // keeping their relative order - which is what should happen to an app
    // installed since the arrangement was made: it appears, rather than
    // silently taking someone else's place.
    int position = 0;
    for (const config::AppPreference& preference : stored) {
        if (registry_.find(preference.id) == nullptr) {
            // An app that no longer exists. Normal rather than exceptional:
            // firmware changes, integrations stop pushing, and a stored order
            // from an older build must not stop a newer one booting.
            continue;
        }

        registry_.move(preference.id, position);
        registry_.setEnabled(preference.id, preference.enabled);

        // Looked up again, after the move.
        //
        // move() erases and reinserts, which shifts every element between the
        // old and new positions - so a pointer taken before it no longer
        // refers to the same app. The previous version of this wrote the
        // duration through such a pointer and put it on whichever app had
        // been shuffled into that slot.
        //
        // It stayed hidden because move() returns early when the app is
        // already where it should be, which is the usual case on a boot where
        // nothing has changed. It surfaced as soon as a script's app was
        // appended at the end and then moved to the front.
        if (app::App* moved = registry_.find(preference.id); moved != nullptr) {
            moved->durationSeconds = preference.durationSeconds;
        }
        ++position;
    }

    logger_.info(lastTickMillis_, "restored app order");
}

bool ApplicationHost::storedOrderMatchesRegistry() const {
    const std::vector<config::AppPreference>& stored = settings_.apps.order;

    std::size_t at = 0;
    for (int i = 0; i < registry_.count(); ++i) {
        const app::App* app = registry_.at(i);
        if (app == nullptr || app->source == app::AppSource::Temporary) {
            continue;  // never recorded, so never compared
        }
        if (at >= stored.size()) {
            return false;
        }
        const config::AppPreference& preference = stored[at];
        if (preference.id != app->id || preference.enabled != app->enabled ||
            preference.durationSeconds != app->durationSeconds) {
            return false;
        }
        ++at;
    }
    return at == stored.size();
}

void ApplicationHost::persistAppOrderIfChanged() {
    // Watched on the registry's own revision rather than hooked into every
    // path that can change it. The API, MQTT and anything added later all
    // mutate the same registry, and one watcher cannot be forgotten by a
    // caller the way five notifications could.
    if (registry_.revision() != persistedAppRevision_) {
        persistedAppRevision_ = registry_.revision();

        // Safe mode deliberately loads no apps, so its registry is not a view
        // of what the user arranged - writing it back would erase the
        // arrangement precisely when the device is least able to explain
        // itself.
        if (bootMode_ != BootMode::Normal) {
            return;
        }

        rememberAppOrder();
        if (!configStore_.save(settings_)) {
            logger_.error(lastTickMillis_, "could not persist app order");
        }
        return;
    }

    // The registry did not change, so if the stored order no longer describes
    // it, the settings changed from somewhere else - a restored backup, or an
    // API call that set the whole document. Those have to reach the live
    // registry or a restore would appear to do nothing until the next reboot,
    // which is the same defect the app duration had.
    if (bootMode_ != BootMode::Normal || settings_.apps.order.empty()) {
        return;
    }
    if (storedOrderMatchesRegistry()) {
        return;
    }
    applyStoredAppOrder();
    // Absorb the moves just made, so this does not read them back as a change
    // the registry made and write the same order out again.
    persistedAppRevision_ = registry_.revision();
}

bool ApplicationHost::glucoseHoldWanted() const noexcept {
    // A sounding alarm holds the screen whatever `pinned` says; without this
    // term the pin it takes would be dropped again on the next tick.
    return (settings_.glucose.pinned && glucoseSource_.configured()) ||
           glucoseAlarm_.sounding();
}

void ApplicationHost::applyGlucoseHold(std::uint64_t nowMillis) {
    const bool pinnedHere = carousel_.isPinned() && carousel_.pinnedId() == kGlucoseAppId;
    if (glucoseHoldWanted()) {
        // Pinned only once the carousel is showing it - by rotation, by the
        // knob, or by the API - so "back" can still leave: the pin does not
        // chase the user, it waits for the app to come round again.
        const app::App* showing = carousel_.active();
        if (!pinnedHere && !carousel_.isPinned() && showing != nullptr &&
            showing->id == kGlucoseAppId) {
            carousel_.pin(kGlucoseAppId, nowMillis);
        }
    } else if (pinnedHere) {
        carousel_.unpin();
    }
}

void ApplicationHost::stepGlucoseFace(int direction) {
    const apps::GlucoseFace current = apps::glucoseFaceFromName(settings_.glucose.face);
    settings_.glucose.face = apps::glucoseFaceName(apps::glucoseFaceStep(current, direction));
    glucoseFaceDirtyMillis_ = lastTickMillis_ == 0 ? 1 : lastTickMillis_;
    // A stale reading draws the no-data face whatever was chosen, so the
    // change would be invisible - and a knob that changes nothing you can see
    // is a broken knob. Name the face instead, the way − / + name a level.
    if (glucoseSource_.reading().stale()) {
        adjustmentShownUntilMillis_ = lastTickMillis_ + kAdjustmentReadoutMillis;
        adjustmentIsVolume_ = false;
        adjustmentIsFace_ = true;
        adjustmentIsSnooze_ = false;
    }
    scheduler_.invalidate();
}

void ApplicationHost::tickGlucoseAlarm(std::uint64_t nowMillis) {
    // Re-told every tick, like the source; configure() only acts on a change.
    glucoseAlarm_.configure(settings_.glucose.alarms);

    const platform::ISystemClock& clock = platform_.clock();
    const bool clockValid = clock.wallClockValid();
    const bool configured = glucoseSource_.configured();
    if (configured && clockValid) {
        if (glucoseFetchableSinceMillis_ == 0) {
            glucoseFetchableSinceMillis_ = nowMillis == 0 ? 1 : nowMillis;
        }
    } else {
        glucoseFetchableSinceMillis_ = 0;
    }

    const apps::glucose::Reading& reading = glucoseSource_.reading();
    apps::glucose::AlarmInputs inputs;
    inputs.nowMillis = nowMillis;
    inputs.configured = configured;
    inputs.fresh = reading.historyCount > 0 && !reading.stale();
    inputs.sgv = reading.sgv;
    if (configured && clockValid) {
        inputs.dataAgeMinutes =
            reading.historyCount > 0
                ? reading.minutesAgo
                : static_cast<int>((nowMillis - glucoseFetchableSinceMillis_) / 60000u);
    }
    if (clockValid) {
        inputs.clockValid = true;
        apps::glucose::localDayAndMinute(clock.unixSeconds(), currentUtcOffsetSeconds(),
                                         inputs.weekday, inputs.localMinutes);
    }

    const apps::glucose::GlucoseAlarm::Decision decision = glucoseAlarm_.tick(inputs);
    if (decision.play) {
        const audio::Melody& melody = glucoseAlarm_.melodyFor(decision.kind);
        platform::IAudioOutput* speaker = platform_.audio();
        const bool played =
            speaker != nullptr && speaker->playMelody(melody, glucoseAlarm_.volumePercent());
        glucoseAlarm_.notePlayed(played, nowMillis);
        if (played) {
            // A little past the melody: the driver still holds a few frames.
            glucoseAlarm_.holdSpeaker(nowMillis +
                                      static_cast<std::uint64_t>(melody.totalMillis()) + 250u);
        }
        if (decision.fresh) {
            std::string line = "glucose alarm: ";
            line += apps::glucose::alarmKindName(decision.kind);
            if (!played) {
                line += " (could not play - no speaker)";
            }
            logger_.warn(nowMillis, line);
        }
        takeScreenForAlarm(nowMillis);
    }
    sharedSpeaker_.holdUntil(glucoseAlarm_.speakerHeldUntil());

    // Drawing follows the alarm even on a dark panel, where nothing else asks
    // for frames: the reading has to appear when it starts, keep up while it
    // sounds, and go when it stops.
    const bool sounding = glucoseAlarm_.sounding();
    if (sounding != glucoseAlarmWasSounding_) {
        glucoseAlarmWasSounding_ = sounding;
        scheduler_.invalidate();
    }
    if (sounding && (glucoseSource_.revision() != lastGlucoseRevision_ ||
                     apps::glucoseChanged(lastClockMillis_, nowMillis))) {
        lastGlucoseRevision_ = glucoseSource_.revision();
        scheduler_.invalidate();
    }
}

void ApplicationHost::takeScreenForAlarm(std::uint64_t nowMillis) {
    if (splashActive_) {
        dismissSplash();
        mapper_.reset();
    }
    if (navigator_.inSettings()) {
        navigator_.exitSettings();
    }
    const bool pinnedHere = carousel_.isPinned() && carousel_.pinnedId() == kGlucoseAppId;
    if (!pinnedHere) {
        const app::App* showing = carousel_.active();
        if (showing == nullptr || showing->id != kGlucoseAppId) {
            transitionDirection_ = render::TransitionDirection::Forward;
            carousel_.activate(kGlucoseAppId, nowMillis);
        }
        carousel_.pin(kGlucoseAppId, nowMillis);
    }
    scheduler_.invalidate();
}

void ApplicationHost::snoozeGlucoseAlarm() {
    if (!glucoseAlarm_.snooze(lastTickMillis_)) {
        return;
    }
    // Quiet now, not at the end of the bar.
    if (platform::IAudioOutput* speaker = platform_.audio()) {
        speaker->stopMelody();
    }
    glucoseAlarm_.holdSpeaker(0);
    sharedSpeaker_.holdUntil(0);

    adjustmentShownUntilMillis_ = lastTickMillis_ + kSnoozeReadoutMillis;
    adjustmentIsSnooze_ = true;
    adjustmentIsVolume_ = false;
    adjustmentIsFace_ = false;
    logger_.info(lastTickMillis_, "glucose alarm snoozed");
    scheduler_.invalidate();
}

void ApplicationHost::persistGlucoseFaceIfChanged() {
    if (glucoseFaceDirtyMillis_ == 0 ||
        lastTickMillis_ - glucoseFaceDirtyMillis_ < kGlucoseFaceSaveDelayMillis) {
        return;
    }
    glucoseFaceDirtyMillis_ = 0;
    if (bootMode_ != BootMode::Normal) {
        return;
    }
    if (!configStore_.save(settings_)) {
        logger_.error(lastTickMillis_, "could not persist glucose face");
    }
}

void ApplicationHost::rememberAppOrder() {
    settings_.apps.order.clear();
    settings_.apps.order.reserve(static_cast<std::size_t>(registry_.count()));

    for (int i = 0; i < registry_.count(); ++i) {
        const app::App* app = registry_.at(i);
        if (app == nullptr) {
            continue;
        }
        // Temporary apps are deliberately left out. They exist for seconds and
        // are gone before the next boot, so recording where they sat would be
        // storing rubbish that outlives them.
        if (app->source == app::AppSource::Temporary) {
            continue;
        }
        config::AppPreference preference;
        preference.id = app->id;
        preference.enabled = app->enabled;
        preference.durationSeconds = app->durationSeconds;
        settings_.apps.order.push_back(std::move(preference));
    }
}

// --- sounds the device makes on its own behalf -------------------------------

void ApplicationHost::announceNotification() {
    const notify::Notification* alert = notifications_.active();
    if (alert == nullptr) {
        // Forgotten deliberately: if the same notification is shown again
        // later it is a new event to the person in the room, and should sound
        // like one.
        announcedSequence_ = 0;
        return;
    }
    if (alert->sequence == announcedSequence_) {
        return;  // already announced; this is the same one still on screen
    }
    announcedSequence_ = alert->sequence;

    platform::IAudioOutput* speaker = platform_.audio() != nullptr ? &sharedSpeaker_ : nullptr;
    if (speaker == nullptr) {
        return;  // no speaker: silently, because absence is reported at boot
    }
    if (sharedSpeaker_.held()) {
        return;  // an alarm is playing; a chime over it is the wrong news
    }

    // A notification may name its own sound; otherwise the configured default
    // applies. "none" is a real choice and the reason this is a string rather
    // than a bool - a clock in a bedroom should be able to say nothing.
    const std::string& sound =
        alert->sound.empty() ? settings_.notifications.sound : alert->sound;
    if (sound.empty() || sound == "none") {
        return;
    }
    if (!speaker->playSound(sound)) {
        // Named a sound this platform does not have. Worth a line in the log
        // rather than silence: the caller believes it asked for something.
        logger_.warn(lastTickMillis_, "unknown notification sound");
    }
}

void ApplicationHost::tickTheClock() {
    if (!settings_.clock.tick || splashActive_) {
        return;
    }

    platform::IAudioOutput* speaker = platform_.audio() != nullptr ? &sharedSpeaker_ : nullptr;
    if (speaker == nullptr) {
        return;
    }

    // Only while the clock is actually on screen, and never over a
    // notification. A device that ticks from inside a drawer, or under an
    // alarm, is a device being annoying for no one's benefit.
    const app::App* active = carousel_.active();
    if (active == nullptr || active->builtin != app::Builtin::Clock ||
        notifications_.active() != nullptr || !settings_.display.power) {
        return;
    }

    const platform::ISystemClock& clock = platform_.clock();
    if (!clock.wallClockValid()) {
        return;  // nothing to tick in time with
    }

    const std::int64_t second = clock.unixSeconds();
    if (second == lastTickedSecond_) {
        return;
    }
    const bool first = lastTickedSecond_ == kNoSecond;
    lastTickedSecond_ = second;
    if (first) {
        return;  // do not tick for the second we happened to arrive in
    }

    // Tick and tock alternate, so a second sounds like a second rather than
    // like a repeated blip. Driven by the clock itself rather than a counter,
    // so a skipped frame cannot swap them permanently.
    speaker->playSound((second & 1) == 0 ? "tick" : "tock");
}

// --- the settings screen ------------------------------------------------------

namespace {

/// Write a non-negative integer into `out`, returning the length. Avoids
/// snprintf in the render path, which blueprint §38 keeps allocation-free.
int writeNumber(char* out, int capacity, int value) noexcept {
    if (capacity < 2) {
        return 0;
    }
    if (value <= 0) {
        out[0] = '0';
        out[1] = '\0';
        return 1;
    }
    char reversed[12];
    int digits = 0;
    while (value > 0 && digits < static_cast<int>(sizeof(reversed))) {
        reversed[digits++] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    if (digits >= capacity) {
        digits = capacity - 1;
    }
    for (int i = 0; i < digits; ++i) {
        out[i] = reversed[digits - 1 - i];
    }
    out[digits] = '\0';
    return digits;
}

/// A bar across the bottom two rows. On 52x16 a number alone is accurate and
/// unreadable at arm's length; the bar is what makes "more" and "less" legible
/// without reading anything.
void drawBar(Canvas& canvas, int permille, Rgb filled, Rgb track) {
    // One row, on the last row. Two rows would eat into the value line, and on
    // a panel this size the bar is the coarse reading anyway - the number
    // above it is the precise one.
    constexpr int kTop = Framebuffer::kHeight - 1;
    canvas.fillRect(Rect{0, kTop, Framebuffer::kWidth, 1}, track);
    if (permille < 0) { permille = 0; }
    if (permille > 1000) { permille = 1000; }
    const int width = (permille * Framebuffer::kWidth) / 1000;
    if (width > 0) {
        canvas.fillRect(Rect{0, kTop, width, 1}, filled);
    }
}

}  // namespace

void ApplicationHost::setNotice(std::string title, std::string detail) {
    noticeTitle_ = std::move(title);
    noticeDetail_ = std::move(detail);
    noticeStartedMillis_ = lastClockMillis_;
    scheduler_.invalidate();
}

void ApplicationHost::clearNotice() noexcept {
    noticeTitle_.clear();
    noticeDetail_.clear();
    noticeStartedMillis_ = 0;
    scheduler_.invalidate();
}

void ApplicationHost::renderNotice(Canvas& canvas, std::uint64_t nowMillis) const {
    const std::uint64_t elapsed =
        nowMillis > noticeStartedMillis_ ? nowMillis - noticeStartedMillis_ : 0;

    // No duration, so no draining rule: this is not a thing that finishes on
    // a schedule the user can watch, and a bar that emptied to nothing would
    // promise one.
    apps::SplashStyle style;
    style.titleColor = colors::kOrange;
    apps::renderSplash(canvas, noticeTitle_, noticeDetail_, elapsed, 0, style);
}

void ApplicationHost::renderHoldCountdown(Canvas& canvas, const char* label,
                                          std::uint64_t remainingMillis,
                                          std::uint64_t holdMillis) const {
    // Counted in whole seconds, rounded up, so the last visible number is 1
    // rather than 0 - a countdown that shows zero and then keeps going reads
    // as stuck.
    const int seconds = static_cast<int>((remainingMillis + 999) / 1000);

    text::TextStyle style;
    style.font = &text::font5x7();
    style.color = colors::kOrange;
    style.hAlign = text::HAlign::Left;
    style.vAlign = text::VAlign::Top;
    text::draw(canvas, label, Rect{1, 0, Framebuffer::kWidth - 2, 7}, style);

    char value[4] = {};
    writeNumber(value, sizeof(value), seconds);

    text::TextStyle number = style;
    number.color = colors::kWhite;
    text::draw(canvas, value, Rect{1, 8, Framebuffer::kWidth - 2, 7}, number);

    // A bar that empties, so the gesture reads as progress rather than as an
    // error message with a number in it.
    const int permille =
        holdMillis == 0 ? 0 : static_cast<int>((remainingMillis * 1000u) / holdMillis);
    drawBar(canvas, permille, colors::kOrange, rgb(30, 30, 30));
}

void ApplicationHost::renderSettings(Canvas& canvas) const {
    // Two lines, not one.
    //
    // The first attempt put the label and the value side by side and the panel
    // showed "BRIGH": at 6 px a character, 52 columns hold eight characters,
    // and "BRIGHT" plus "184" is nine. The fix is not a shorter word - naming a
    // setting "BRT" to fit a layout is the layout winning an argument it should
    // not be in - it is to stop asking one row to hold both.
    //
    // Label on the top line, value on the second, bar on the last row. Each
    // line now has the full width, so every setting name fits at its real
    // length and a three-digit value has room beside nothing.
    const input::SettingSlot slot = navigator_.current();

    text::TextStyle label;
    label.font = &text::font5x7();
    label.color = colors::kWhite;
    label.hAlign = text::HAlign::Left;
    label.vAlign = text::VAlign::Top;
    text::draw(canvas, input::settingLabel(slot), Rect{1, 0, Framebuffer::kWidth - 2, 7}, label);

    char value[10] = {};
    int permille = -1;
    Rgb accent = colors::kCyan;

    switch (slot) {
        case input::SettingSlot::Brightness: {
            const int level = static_cast<int>(settings_.display.brightness);
            writeNumber(value, sizeof(value), level);
            permille = (level * 1000) / 255;
            break;
        }
        case input::SettingSlot::Overlay: {
            const char* name =
                render::overlayName(render::overlayFromName(settings_.display.overlay));
            int at = 0;
            // Upper-cased into the fixed buffer: the font has one case, and the
            // stored names are lower-case because config files are read by
            // people too.
            for (; name[at] != 0 && at < static_cast<int>(sizeof(value)) - 1; ++at) {
                const char c = name[at];
                value[at] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
            }
            break;
        }
        case input::SettingSlot::Volume: {
            const int percent = static_cast<int>(settings_.audio.volumePercent);
            const int digits = writeNumber(value, sizeof(value), percent);
            if (digits > 0 && digits < static_cast<int>(sizeof(value)) - 1) {
                value[digits] = '%';
                value[digits + 1] = 0;
            }
            permille = percent * 10;
            break;
        }
        case input::SettingSlot::Count:
            break;
    }

    text::TextStyle reading = label;
    reading.color = accent;
    text::draw(canvas, value, Rect{1, 8, Framebuffer::kWidth - 2, 7}, reading);

    if (permille >= 0) {
        drawBar(canvas, permille, accent, rgb(30, 30, 30));
    }
}

void ApplicationHost::renderAdjustment(Canvas& canvas) const {
    // The same two lines the settings screen uses, over the app.
    //
    // The first version wrote a bare number into the bottom six rows with no
    // label, which answered "something changed" and not "what". Reusing the
    // settings layout means one visual language for adjustment on this device:
    // whatever is being changed, it reads the same whether you got there by
    // holding the knob or by tapping a button.
    if (adjustmentIsSnooze_) {
        canvas.fillRect(Framebuffer::bounds(), colors::kBlack);
        text::TextStyle label;
        label.font = &text::font5x7();
        label.color = colors::kWhite;
        label.hAlign = text::HAlign::Left;
        label.vAlign = text::VAlign::Top;
        text::draw(canvas, "SNOOZE", Rect{1, 0, Framebuffer::kWidth - 2, 7}, label);
        // Eight 5x7 glyphs fill the panel: "120 MIN" fits, and "until it
        // leaves the range" has to be said in fewer.
        char value[10] = {};
        const int minutes = glucoseAlarm_.snoozeMinutes();
        if (minutes == 0) {
            std::snprintf(value, sizeof value, "TIL OK");
        } else {
            std::snprintf(value, sizeof value, "%d MIN", minutes);
        }
        text::TextStyle reading = label;
        reading.color = colors::kCyan;
        text::draw(canvas, value, Rect{1, 8, Framebuffer::kWidth - 2, 7}, reading);
        return;
    }

    const bool volume = adjustmentIsVolume_;
    const bool face = adjustmentIsFace_;

    const int level = volume ? static_cast<int>(settings_.audio.volumePercent)
                             : static_cast<int>(settings_.display.brightness);
    const int permille = face ? -1 : volume ? level * 10 : (level * 1000) / 255;

    // Cleared rather than blended. This is a momentary interruption, and half
    // an app showing through the digits is harder to read than either alone.
    canvas.fillRect(Framebuffer::bounds(), colors::kBlack);

    text::TextStyle label;
    label.font = &text::font5x7();
    label.color = colors::kWhite;
    label.hAlign = text::HAlign::Left;
    label.vAlign = text::VAlign::Top;
    text::draw(canvas, face ? "FACE" : volume ? "VOLUME" : "BRIGHT",
               Rect{1, 0, Framebuffer::kWidth - 2, 7}, label);

    char value[10] = {};
    if (face) {
        // Short words, because eight 5x7 glyphs is the width of the panel and
        // "hero-delta" is ten. Enough to tell the faces apart by name.
        const char* word = "HERO";
        switch (apps::glucoseFaceFromName(settings_.glucose.face)) {
            case apps::GlucoseFace::HeroDelta: word = "DELTA"; break;
            case apps::GlucoseFace::HeroGraph: word = "GRAPH"; break;
            case apps::GlucoseFace::Clock: word = "CLOCK"; break;
            case apps::GlucoseFace::BigGraph: word = "BIG"; break;
            case apps::GlucoseFace::Hero:
            case apps::GlucoseFace::NoData: break;
        }
        std::snprintf(value, sizeof value, "%s", word);
    } else {
        const int digits = writeNumber(value, sizeof(value), level);
        if (volume && digits > 0 && digits < static_cast<int>(sizeof(value)) - 1) {
            value[digits] = '%';
            value[digits + 1] = 0;
        }
    }

    text::TextStyle reading = label;
    reading.color = colors::kCyan;
    text::draw(canvas, value, Rect{1, 8, Framebuffer::kWidth - 2, 7}, reading);

    drawBar(canvas, permille, colors::kCyan, rgb(30, 30, 30));
}

// --- the loop ----------------------------------------------------------------

bool ApplicationHost::splashElapsed(std::uint64_t nowMillis) const noexcept {
    if (!ticking_) {
        return false;
    }
    if (nowMillis < firstTickMillis_) {
        return true;  // clock stepped back; do not strand the splash on screen
    }
    return (nowMillis - firstTickMillis_) >= config_.splashMillis;
}

bool ApplicationHost::tick(std::uint64_t nowMillis) {
    if (!initialized_ || shutdownRequested_) {
        return false;
    }
    lastTickMillis_ = nowMillis;
    sharedSpeaker_.setNow(nowMillis);

    if (!ticking_) {
        ticking_ = true;
        firstTickMillis_ = nowMillis;
    }

    pumpInput(nowMillis);
    // Before anything renders, so a script and the clock app beside it never
    // disagree about what time it is within one frame.
    publishScriptEnvironment();

    persistIconsIfChanged();
    persistScriptsIfChanged();
    persistAppOrderIfChanged();
    persistGlucoseFaceIfChanged();
    applyCarouselSettings();
    applyTimeSettings();
    applyBrightness();
    // Settings reach the host by being written into settings_, so the source
    // is re-told every tick; configure() only acts on a change.
    glucoseSource_.configure(settings_.glucose.url, settings_.glucose.apiSecretSha1,
                             settings_.glucose.pollSeconds);

    if (rescue_.tick(nowMillis)) {
        performRescue();
    }

    if (setupHold_.tick(nowMillis)) {
        // The in-progress press is discarded here, and that is the whole
        // reason this gesture can share a button with SettingsToggle: the
        // mapper decides long-versus-short on release, so with no press left
        // to release, letting go of the knob does nothing at all.
        mapper_.reset();
        performSetupRequest();
    }

    // The countdown has to ask for its own frames.
    //
    // Redrawing normally stops while the panel is off - otherwise a dark panel
    // would re-render black at the full frame rate - and the rescue screen is
    // drawn precisely then. Invalidated once per displayed second rather than
    // every tick: a countdown needs thirty frames a second about as much as a
    // dark panel does.
    const int rescueSecond = rescue_.counting()
        ? static_cast<int>((rescue_.remainingMillis(nowMillis) + 999) / 1000)
        : -1;
    if (rescueSecond != lastRescueSecond_) {
        lastRescueSecond_ = rescueSecond;
        scheduler_.invalidate();
    }

    const int setupSecond = setupHold_.counting()
        ? static_cast<int>((setupHold_.remainingMillis(nowMillis) + 999) / 1000)
        : -1;
    if (setupSecond != lastSetupSecond_) {
        lastSetupSecond_ = setupSecond;
        scheduler_.invalidate();
    }

    if (splashActive_) {
        if (splashElapsed(nowMillis)) {
            splashActive_ = false;
            logger_.info(nowMillis, "splash finished");
            scheduler_.invalidate();
        } else {
            // Recomputed, not remembered.
            //
            // This was worked out once at startup - before the radio had
            // associated and before DHCP had a lease - so it baked in
            // "no Wi-Fi" and went on saying it while the device sat happily
            // on the network serving this very page. Reported from hardware
            // several times as "no wifi, but the web config loads", which is
            // exactly what a stale string looks like from the outside.
            //
            // Association takes ten to twenty seconds on this hardware and
            // the splash is on screen for about that long, so this is the one
            // place the answer genuinely changes while it is being shown.
            splashDetail_ = apps::splashDetail(kVersion, platform_.network());
            splashAddress_ = apps::splashAddress(platform_.network());

            // The detail line scrolls, so every frame differs.
            scheduler_.invalidate();
        }
    }

    if (!splashActive_ && bootMode_ == BootMode::Normal) {
        // Settings are shared by pointer with the API, so the display can be
        // switched off between two ticks. Noticing it here rather than at the
        // call site means every future route to the setting — MQTT, buttons, a
        // schedule — gets the redraw for free.
        if (settings_.display.power != renderedWithPower_) {
            renderedWithPower_ = settings_.display.power;
            scheduler_.invalidate();
        }

        // Timekeeping continues while the panel is off — apps still rotate and
        // notifications still expire — so switching it back on shows the present
        // moment rather than a resumed backlog.
        // Sampled every tick regardless of which app is showing, so switching
        // to the visualiser mid-sound shows what just happened rather than
        // starting from an empty panel.
        if (platform::IMicrophone* microphone = platform_.microphone()) {
            const platform::SoundLevel sound = microphone->level();
            if (sound.known) {
                // One column per kVisualizerSampleMillis, not one per frame.
                //
                // Pushing every tick scrolled the trace at the frame rate: 52
                // columns crossed the panel in under two seconds, which reads
                // as frantic rather than as a room. It also stuttered, because
                // the microphone reports at about 22 Hz and a faster loop just
                // duplicated the last reading.
                //
                // The peak between pushes is kept rather than the latest
                // reading, so slowing the trace down cannot swallow a handclap
                // that happened between two columns.
                if (sound.amplitude > visualizerPeak_) {
                    visualizerPeak_ = sound.amplitude;
                }
                if (nowMillis - lastVisualizerPushMillis_ >= kVisualizerSampleMillis) {
                    visualizer_.push(visualizerPeak_);
                    visualizerPeak_ = 0;
                    lastVisualizerPushMillis_ = nowMillis;
                }
            }
        }

        // Frozen while settings are open.
        //
        // Without this the carousel kept advancing underneath the menu, which
        // did two visible things and one confusing one: every few seconds a
        // transition started and slid the settings screen sideways like an app,
        // and leaving settings landed on whatever app the timer had reached
        // rather than the one the user left. Between them it read as though
        // settings were a page in the rotation, which is precisely what it is
        // not - it is a mode on top of the rotation, and a mode that keeps
        // moving is not a mode.
        // Time in a menu is not time on screen.
        //
        // Held still every tick rather than reset on the way out, so no exit
        // path can forget - and held for one tick *after* leaving too, because
        // input is processed earlier in this same tick. Without that, the tick
        // that closes settings is also the first tick that counts dwell, and
        // it counts every millisecond since the last one.
        const bool inSettings = navigator_.inSettings();
        bool carouselMoved = false;
        if (inSettings || wasInSettings_) {
            carousel_.restartDwell(nowMillis);
        }
        wasInSettings_ = inSettings;

        // A running stopwatch holds the screen.
        //
        // Starting one and then watching the carousel carry it away eight
        // seconds later would make the feature useless, and pausing the
        // rotation by hand first is a step nobody should have to know about.
        // It is only held while the stopwatch is both running *and* the app
        // on screen, so a stopwatch left running in the background does not
        // freeze the rotation.
        const app::App* showing = carousel_.active();
        const bool stopwatchHolding =
            stopwatch_.running() && showing != nullptr &&
            showing->builtin == app::Builtin::Stopwatch;

        if (!inSettings && !stopwatchHolding) {
            carouselMoved = carousel_.tick(nowMillis);
            applyGlucoseHold(nowMillis);
        } else if (stopwatchHolding) {
            // Kept fresh so the app does not vanish the instant it stops.
            carousel_.restartDwell(nowMillis);
        }
        const bool notificationsMoved = notifications_.tick(nowMillis);

        announceNotification();
        tickTheClock();

        // Only the redrawing stops. Without this a dark panel would re-render
        // black at the full frame rate, which is the one thing an off switch is
        // supposed to avoid.
        if (settings_.display.power) {
            if (carouselMoved || notificationsMoved) {
                scheduler_.invalidate();
            }

            // A rotation that happened on its own always reads as forward. A
            // knob turn sets the direction before calling next()/previous(),
            // and beginTransition keeps whichever was set most recently.
            if (carouselMoved && !splashActive_) {
                beginTransition(nowMillis, transitionDirection_);
            }

            // A transition is motion by definition, so it has to keep asking
            // for frames for as long as it runs - dirty tracking would
            // otherwise freeze it on its first step.
            if (transitionRunning(nowMillis)) {
                scheduler_.invalidate();
            }

            // Weather moves, so dirty tracking must not freeze it. Checked at
            // the frame interval rather than every tick, since that is the
            // fastest it could usefully change anyway.
            if (settings_.display.overlay != "none") {
                scheduler_.invalidate();
            }

            // A readout that has just expired has to be painted over, whatever
            // sits beneath it. The clock underneath redraws every second and
            // hid this; a static face does not, and kept the readout up until
            // its next minute.
            if (adjustmentShownUntilMillis_ != 0 && nowMillis >= adjustmentShownUntilMillis_) {
                adjustmentShownUntilMillis_ = 0;
                scheduler_.invalidate();
            }

            // Anything time-varying has to say so, or dirty tracking would leave
            // it frozen between content changes.
            if (notifications_.active() != nullptr) {
                scheduler_.invalidate();
            } else if (const app::App* active = carousel_.active()) {
                if (active->builtin == app::Builtin::TestPattern) {
                    scheduler_.invalidate();
                } else if (active->builtin == app::Builtin::Visualizer) {
                    // Sound does not wait for a redraw to be due.
                    scheduler_.invalidate();
                } else if (active->builtin == app::Builtin::Stopwatch) {
                    // Tenths, so ten frames a second - not thirty. The last
                    // digit is the only thing moving and it changes at 10 Hz;
                    // anything faster would be redrawing an identical panel.
                    if ((nowMillis / 100u) != (lastClockMillis_ / 100u)) {
                        scheduler_.invalidate();
                    }
                } else if (active->builtin == app::Builtin::Battery) {
                    // Once a second is ample for a value that moves a percent
                    // an hour, and still far more responsive than the panel
                    // needs. Without it the card would freeze at whatever the
                    // charge was when it first drew.
                    if ((nowMillis / 1000u) != (lastClockMillis_ / 1000u)) {
                        scheduler_.invalidate();
                    }
                } else if (active->builtin == app::Builtin::Script) {
                    // A script gets the frame time and may use it, and there
                    // is no way to know whether it did. Dirty tracking needs
                    // the content to declare that it moves; a script cannot,
                    // so the safe answer is that it always might.
                    scheduler_.invalidate();
                } else if (active->builtin == app::Builtin::Clock) {
                    if (apps::clockChanged(platform_.clock(), clockStyle(),
                                           lastClockMillis_, nowMillis)) {
                        scheduler_.invalidate();
                    }
                } else if (active->builtin == app::Builtin::Glucose) {
                    // A new reading, or the minute rolling over (age pips, the
                    // stale rule, the clock face); nothing here moves faster.
                    if (glucoseSource_.revision() != lastGlucoseRevision_ ||
                        apps::glucoseChanged(lastClockMillis_, nowMillis)) {
                        lastGlucoseRevision_ = glucoseSource_.revision();
                        scheduler_.invalidate();
                    }
                } else if (refreshActiveScene() && scene_.animates()) {
                    scheduler_.invalidate();
                }
            }
        }
    }

    // MQTT runs off the same loop as everything else, so nothing arrives on a
    // thread the rest of the firmware does not know about.
    if (bootMode_ == BootMode::Normal) {
        mqtt::MqttService::DeviceState state;
        if (const app::App* active = carousel_.active()) {
            state.activeAppId = active->id;
        }
        state.healthy = healthy_;
        if (platform_.network() != nullptr) {
            const platform::NetworkStatus status = platform_.network()->status();
            state.rssiDbm = status.rssiDbm;
            state.hasRssi = status.connected;
        }
        mqtt_.setDeviceState(std::move(state));
        mqtt_.tick(nowMillis);

        // Same loop, same reason. One request at a time, and the schedule
        // decides which - see ScriptFetcher. The glucose source goes first:
        // both share the one client and each waits while the other's request
        // is in flight, so order only decides who wins a tie.
        const bool networkUp =
            platform_.network() != nullptr && platform_.network()->status().connected;
        const platform::ISystemClock& systemClock = platform_.clock();
        glucoseSource_.setNetworkUp(networkUp);
        glucoseSource_.tick(nowMillis, systemClock.unixSeconds(), systemClock.wallClockValid(),
                            currentUtcOffsetSeconds());
        tickGlucoseAlarm(nowMillis);
        fetcher_.setNetworkUp(networkUp);
        fetcher_.tick(nowMillis);
    }

    // Settings cannot outlive the user's attention: someone who walks away
    // mid-adjustment would otherwise leave a clock showing "BRIGHT 168".
    if (navigator_.tick(nowMillis)) {
        logger_.info(nowMillis, "settings closed after idle");
        scheduler_.invalidate();
    }

    if (scheduler_.beginFrame(nowMillis)) {
        const std::uint64_t startedAt = platform_.clock().monotonicMillis();

        renderFrame(nowMillis);

        // Over the app, under the transition. Additive, so it only lights
        // pixels the app left dark - a raindrop passes behind the digits
        // rather than through them (DESIGN.md section 7).
        if (settings_.display.power && !splashActive_ && !navigator_.inSettings()) {
            const render::Overlay overlay =
                render::overlayFromName(settings_.display.overlay);
            if (overlay != render::Overlay::None) {
                render::drawOverlay(framebuffer_, overlay, nowMillis);
            }
        }

        // The transient readout after − or + while browsing. Composited here
        // rather than inside renderFrame because every branch of that function
        // returns as soon as it has drawn, and a readout that only appeared
        // over some apps would be worse than none.
        if (adjustmentShownUntilMillis_ > nowMillis && !navigator_.inSettings() &&
            settings_.display.power && !splashActive_) {
            Canvas readout(framebuffer_);
            renderAdjustment(readout);
        }

        // Composited after rendering, never during it. renderFrame only ever
        // draws the app that is active now; the outgoing frame was captured
        // when the change happened, so nothing in the renderer knows a
        // transition exists.
        // An overlay owns the whole panel, so it must not be slid.
        //
        // renderFrame returns early for these, but compositing happens out
        // here and did not know that - so a setup notice raised while the
        // carousel happened to be mid-rotation was blended with the outgoing
        // app and slid off the screen like the next item in the list. Seen on
        // hardware: the setup instructions animated away while somebody was
        // reading them.
        //
        // Checked here rather than by cancelling the transition when a notice
        // is raised, because the carousel keeps running underneath and would
        // simply start another one on its next advance.
        const bool overlayOwnsPanel =
            showingNotice() || rescue_.counting() || setupHold_.counting();

        if (transitionRunning(nowMillis) && !overlayOwnsPanel) {
            transitionScratch_ = framebuffer_;
            const std::uint64_t elapsed = nowMillis - transitionStartMillis_;
            const int permille = static_cast<int>(
                (elapsed * 1000u) / render::kTransitionMillis);
            render::composite(framebuffer_, previousFrame_, transitionScratch_,
                              transitionStyle_, transitionDirection_, permille);
        } else {
            transitionActive_ = false;
        }

        platform_.display().present(framebuffer_);

        const std::uint64_t finishedAt = platform_.clock().monotonicMillis();
        scheduler_.endFrame(nowMillis, static_cast<std::uint32_t>(finishedAt - startedAt));
        lastClockMillis_ = nowMillis;

        ++framesSinceBoot_;
    }

    // Health is a frame count OR an elapsed time, whichever lands first. With
    // dirty rendering a static screen may draw exactly once and then legitimately
    // stop, so waiting on frames alone would never mark it healthy.
    if (!healthy_ && framesSinceBoot_ > 0) {
        const bool enoughFrames = framesSinceBoot_ >= config_.healthyAfterFrames;
        const bool enoughTime = nowMillis >= firstTickMillis_ &&
                                (nowMillis - firstTickMillis_) >= config_.healthyAfterMillis;
        if (enoughFrames || enoughTime) {
            markHealthy();
        }
    }

    return true;
}

std::uint64_t ApplicationHost::nextDueMillis(std::uint64_t nowMillis) const {
    return scheduler_.nextDueMillis(nowMillis);
}

// --- rendering ---------------------------------------------------------------

apps::ClockStyle ApplicationHost::clockStyle() const noexcept {
    // Starts from the build-time style so anything the user has not chosen keeps
    // whatever this build considers sensible, then applies stored settings over
    // the top. Names that are not recognised fall back inside the converters.
    apps::ClockStyle style = config_.clock;
    style.theme = apps::clockThemeFromName(settings_.clock.theme);
    style.twentyFourHour = settings_.clock.twentyFourHour;
    style.leadingZero = settings_.clock.leadingZero;
    style.showAmPm = settings_.clock.showAmPm;
    style.color = fromPacked(settings_.clock.color);
    style.accentColor = fromPacked(settings_.clock.accentColor);
    style.dateColor = fromPacked(settings_.clock.dateColor);
    style.dateOrder = apps::dateOrderFromName(settings_.clock.dateOrder);
    style.dateSeparator = apps::dateSeparatorFromName(settings_.clock.dateSeparator);
    style.dateYear = apps::dateYearFromName(settings_.clock.dateYear);
    style.blinkPeriodMillis = settings_.clock.blinkPeriodMillis;
    style.utcOffsetSeconds = currentUtcOffsetSeconds();
    return style;
}

void ApplicationHost::applyTimeSettings() {
    // Parsed once per change rather than once per frame. clockStyle() runs on
    // every render, and re-parsing a rule string to get the same answer sixty
    // times a second would be work for nothing - and it is why this is a tick
    // job rather than something clockStyle() does for itself.
    if (settings_.clock.timezone == timezoneSpec_) {
        return;
    }
    timezoneSpec_ = settings_.clock.timezone;
    timezoneValid_ = timezone_::Timezone::parse(timezoneSpec_, timezone_);

    if (timezoneSpec_.empty()) {
        return;
    }
    if (!timezoneValid_) {
        // Worth saying out loud. A rule that does not parse falls back to the
        // stored offset, and ignoring it silently would leave somebody certain
        // they had set a timezone and puzzled twice a year.
        logger_.warn(lastTickMillis_, "timezone rule not understood; using the fixed offset");
        return;
    }
    logger_.info(lastTickMillis_,
                 timezone_.observesDaylight() ? "timezone set, with daylight saving"
                                              : "timezone set, fixed offset");
}

int ApplicationHost::currentUtcOffsetSeconds() const {
    // The rule wins where there is one, and the stored offset is the fallback
    // for the large part of the world that does not observe daylight saving -
    // and for every device configured before timezones existed.
    if (!timezoneValid_) {
        return settings_.clock.utcOffsetSeconds;
    }
    const platform::ISystemClock& clock = platform_.clock();
    if (!clock.wallClockValid()) {
        // Without a date there is no way to know which side of a changeover we
        // are on, so the standard offset is the honest answer rather than a
        // coin toss.
        return timezone_.standardOffsetSeconds();
    }
    return timezone_.offsetSeconds(clock.unixSeconds());
}

bool ApplicationHost::refreshActiveScene() {
    const app::App* active = carousel_.active();
    if (active == nullptr || active->builtin != app::Builtin::None) {
        return false;
    }

    // Re-parse only when the app or the registry changed. The Scene holds views
    // into the app's JSON, so both must match before it is safe to render.
    // Icon changes matter too: a scene validated against a missing icon must be
    // revalidated once that icon exists.
    const std::uint32_t revision = registry_.revision() ^ (icons_.revision() << 16);
    if (parsedAppId_ != active->id || parsedRevision_ != revision) {
        sceneReady_ = scene_.load(active->sceneJson);
        parsedAppId_ = active->id;
        parsedRevision_ = revision;

        if (!sceneReady_) {
            logger_.warn(lastTickMillis_, "active app has an unusable scene");
        } else if (scene_.issueCount() > 0) {
            // The first issue, with the element it belongs to. Saying only
            // that something was wrong leaves somebody pushing scenes from an
            // integration to guess which of them, and why.
            const scene::Issue& first = scene_.issueAt(0);
            std::string detail = active->id;
            detail += ": element ";
            detail += std::to_string(first.elementIndex);
            detail += ": ";
            detail += first.message;
            logger_.warn(lastTickMillis_, detail);
        }
    }
    return sceneReady_;
}

void ApplicationHost::renderSafeMode() {
    Canvas canvas(framebuffer_);
    canvas.clear();

    text::TextStyle style;
    style.font = &text::font5x7();
    style.color = rgb(255, 80, 40);
    style.hAlign = text::HAlign::Center;
    style.vAlign = text::VAlign::Top;
    text::draw(canvas, "SAFE", Rect{0, 0, Framebuffer::kWidth, 7}, style);

    style.color = rgb(120, 120, 120);
    text::draw(canvas, "MODE", Rect{0, 9, Framebuffer::kWidth, 7}, style);
}

void ApplicationHost::renderFrame(std::uint64_t nowMillis) {
    if (bootMode_ == BootMode::SafeMode) {
        renderSafeMode();
        return;
    }

    Canvas canvas(framebuffer_);
    canvas.clear();

    // The rescue countdown outranks everything, including panel power.
    //
    // Somebody holding both buttons has either meant to and needs to see it
    // working, or has not and needs a reason to stop. A device that stayed
    // dark and then silently cleared its own password would be
    // indistinguishable from one that crashed - and this gesture is reached
    // for precisely when nothing else about the device is behaving.
    if (rescue_.counting()) {
        renderHoldCountdown(canvas, "RESET", rescue_.remainingMillis(nowMillis),
                            input::Rescue::kHoldMillis);
        return;
    }

    // Second, so a hand holding everything at once sees the more destructive
    // of the two. Nobody should discover they were three seconds from
    // clearing their password because the panel was showing the other thing.
    if (setupHold_.counting()) {
        renderHoldCountdown(canvas, "SETUP", setupHold_.remainingMillis(nowMillis),
                            input::SetupHold::kHoldMillis);
        return;
    }

    // A notice outranks settings, panel power and the splash, and is second
    // only to the rescue countdown.
    //
    // It is set when the device has taken its own radio to host an access
    // point, so the panel is the only way left to say what that access point
    // is called and where to find it. Honouring "display off" here would mean
    // a device that unreachably hosts a network nobody can name.
    if (showingNotice()) {
        renderNotice(canvas, nowMillis);
        return;
    }

    // Settings come before the power check, deliberately.
    //
    // Panel power is one of the settings, so honouring "off" while the menu is
    // open would black out the only screen showing the control that turns it
    // back on - a trap with no way out except the web UI. The panel goes dark
    // when settings are left, which is also when the user can see it happen.
    if (navigator_.inSettings()) {
        renderSettings(canvas);
        return;
    }

    // Display off. The panel is cleared and still presented, so it goes properly
    // dark rather than freezing on whatever was last drawn. Safe mode is checked
    // first on purpose: a stored `power: false` must never be able to hide the
    // reason the device ended up in safe mode.
    // A sounding alarm shows its reading even on a switched-off panel and over
    // a notification: an alarm about a number nobody can see is half an alarm.
    // Drawn, not switched on - display.power is never written.
    if (glucoseAlarm_.sounding() &&
        (!settings_.display.power || notifications_.active() != nullptr)) {
        const apps::glucose::Reading& reading = glucoseSource_.reading();
        apps::renderGlucose(canvas, reading,
                            reading.stale() ? apps::GlucoseFace::NoData
                                            : apps::glucoseFaceFromName(settings_.glucose.face));
        return;
    }

    if (!settings_.display.power) {
        return;
    }

    if (splashActive_) {
        apps::renderSplash(canvas, "STIPPLE", kVersion, nowMillis - firstTickMillis_,
                           config_.splashMillis, config_.splash, splashAddress_);
        return;
    }

    // Notifications take the whole panel: they interrupt rather than share.
    if (const notify::Notification* alert = notifications_.active()) {
        notify::render(canvas, *alert, Framebuffer::bounds(),
                       notifications_.activeElapsedMillis(nowMillis), &icons_);
        return;
    }

    const app::App* active = carousel_.active();
    if (active == nullptr) {
        return;  // nothing enabled; a blank panel is the honest result
    }

    switch (active->builtin) {
        case app::Builtin::Clock:
            apps::renderClock(canvas, platform_.clock(), clockStyle());
            return;
        case app::Builtin::Visualizer: {
            // A present IMicrophone is not the same as a working one, and on
            // the TC002 the difference is the whole bug: the adapter offers
            // itself as a microphone as soon as the MCU serial port opens,
            // then never receives an audio frame. The pointer check passed,
            // no sample was ever pushed, and the app drew its baseline - a
            // flat line that reads as a silent room rather than as a device
            // that cannot hear.
            //
            // Asking whether anything has actually been heard covers both
            // cases honestly. ADR 0013 is about exactly this: absence should
            // be visible, not dressed up as a plausible value.
            if (platform_.microphone() == nullptr || !visualizer_.hasSamples()) {
                apps::renderNoMicrophone(canvas, colors::kWhite);
                return;
            }
            apps::VisualizerStyle visualizerStyle;
            visualizerStyle.kind =
                apps::visualizerStyleFromName(settings_.visualizer.style);
            visualizer_.render(canvas, visualizerStyle, nowMillis);
            return;
        }
        case app::Builtin::Battery: {
            platform::BatteryStatus status;
            if (platform::IPowerSource* power = platform_.power()) {
                status = power->battery();
            }
            apps::renderBattery(canvas, status, apps::BatteryStyle{});
            return;
        }
        case app::Builtin::Stopwatch:
            apps::renderStopwatch(canvas, stopwatch_, nowMillis, apps::StopwatchStyle{});
            return;
        case app::Builtin::Glucose: {
            // A stale reading shows the explicit no-data face whatever face is
            // chosen: a grey "---" on the hero face reads as a value that is
            // merely dim, and this display exists to be read across a room.
            const apps::glucose::Reading& reading = glucoseSource_.reading();
            apps::renderGlucose(canvas, reading,
                                reading.stale() ? apps::GlucoseFace::NoData
                                                : apps::glucoseFaceFromName(settings_.glucose.face));
            return;
        }
        case app::Builtin::TestPattern:
            demo::drawTestPattern(canvas, static_cast<int>(nowMillis / 33u));
            return;
        case app::Builtin::Script: {
            // A script app whose script cannot run says so. Going black would
            // be indistinguishable from a script that draws nothing, from a
            // crashed device, and from a panel with a dead row - and the
            // author is the one person who can fix it and the one person who
            // would be left guessing. ADR 0013.
            if (scripts_ == nullptr) {
                apps::renderUnavailable(canvas, "NO", "SCRIPTS");
                return;
            }
            if (scripts_->draw(active->id, canvas, carousel_.dwellMillis(nowMillis))) {
                return;
            }
            // The reason does not fit on 52 pixels and is not thrown away -
            // it is in the script's `problem`, which the API and the web UI
            // both show next to the code that caused it.
            apps::renderUnavailable(canvas, "SCRIPT", scripts_->has(active->id) ? "ERROR" : "?");
            return;
        }
        case app::Builtin::None:
            break;
    }

    if (refreshActiveScene()) {
        // A scene whose elements all failed validation says so.
        //
        // It used to render black, which is indistinguishable from a working
        // app with nothing to show, from a crashed device and from a dead
        // panel. A Domoticz push produced exactly that: every element
        // rejected, the load still successful, and no way to tell from the
        // outside. ADR 0013.
        if (!scene_.anyRenderable()) {
            apps::renderUnavailable(canvas, "EMPTY", "SCENE");
            return;
        }

        // Scroll position is measured from when the app appeared, so each app
        // starts reading from the beginning of its text.
        scene_.render(canvas, carousel_.dwellMillis(nowMillis));
    }
}

void ApplicationHost::beginTransition(std::uint64_t nowMillis,
                                      render::TransitionDirection direction) {
    if (!settings_.apps.transitions) {
        transitionActive_ = false;
        return;
    }

    // The frame already on the panel becomes the outgoing one. Capturing it
    // here is what lets the renderer stay ignorant of transitions entirely: it
    // only ever draws the app that is active now.
    previousFrame_ = framebuffer_;
    transitionStartMillis_ = nowMillis;
    transitionDirection_ = direction;
    // Notifications arrive rather than rotate, so they always fade; the
    // carousel uses whatever the user chose (DESIGN.md section 6).
    //
    // The notification case is deliberately not configurable. A slide would
    // say the notification is simply the next item in the rotation, which is a
    // statement about what a notification *is* rather than a preference - and
    // getting it wrong makes an alarm look like an app.
    transitionStyle_ = notifications_.active() != nullptr
                           ? render::TransitionStyle::Fade
                           : render::transitionStyleFromName(settings_.apps.transition);
    transitionActive_ = true;
}

bool ApplicationHost::transitionRunning(std::uint64_t nowMillis) const noexcept {
    if (!transitionActive_) {
        return false;
    }
    return nowMillis - transitionStartMillis_ < render::kTransitionMillis;
}

// --- API ---------------------------------------------------------------------

api::Response ApplicationHost::handle(const api::Request& request) {
    // One gate, ahead of both the page and the API, because they are the same
    // server and two schemes would be two things to get wrong (ADR 0018).
    //
    // Off by default: a device that demanded a password before it would show
    // a clock would be a worse first five minutes than the risk it removes.
    // It is offered during first run rather than left to be discovered.
    //
    // Everything is behind it, with no carve-out for /health or the static
    // page. An exception is a thing to remember, and the one nobody
    // remembers is the one that matters - a device whose panel can be
    // rewritten through the endpoint somebody decided was harmless.
    if (!settings_.web.username.empty() &&
        !api::basicAuthorised(request.authorization, settings_.web.username,
                              settings_.web.password)) {
        api::Response denied = api::error(401, "unauthorized", "authentication required");
        // Without this a browser shows a bare error page and the person has
        // no way to supply what is missing.
        denied.wwwAuthenticate = "Basic realm=\"STIPPLE\", charset=\"UTF-8\"";
        return denied;
    }

    // The configuration UI is tried first, but only for paths the API does not
    // own. Ordering it this way means a future asset called "api" could never
    // shadow an endpoint, and an unknown /api/v1 path still gets the API's own
    // 404 rather than a confusing "no such page".
    if (request.path.rfind("/api/", 0) != 0) {
        api::Response staticResponse;
        if (staticFiles_.tryHandle(request, staticResponse)) {
            return staticResponse;
        }
    }

    const api::Response response = apiServer_.handle(request, lastTickMillis_);

    // Any successful change ends the first run.
    //
    // A state, not a wizard: there is no "finish setup" button, because a
    // button somebody has to find is a step that can be missed. Hooking the
    // save calls instead would have missed this entirely - the API persists
    // through its own handle on the store.
    if (request.method != api::Method::Get && response.status < 400) {
        firstRun_ = false;
    }

    // Settings are shared by pointer with the API, so a PATCH may have pointed
    // MQTT at a different broker. Re-reading is cheap and does nothing when
    // nothing relevant changed.
    if (request.method != api::Method::Get && response.status < 400 &&
        bootMode_ == BootMode::Normal) {
        mqtt_.configure(lastTickMillis_);
        mqtt_.invalidateStatus();
    }

    // Log what changed the device and what failed, but not routine reads. A
    // dashboard polling /health every second would otherwise push everything
    // worth seeing out of a 24-entry ring within half a minute.
    const bool mutating = request.method != api::Method::Get;
    if (mutating || response.status >= 400) {
        std::string line = api::methodName(request.method);
        line += ' ';
        line += request.path;
        line += ' ';
        line += std::to_string(response.status);

        if (response.status >= 500) {
            logger_.error(lastTickMillis_, line);
        } else if (response.status >= 400) {
            logger_.warn(lastTickMillis_, line);
        } else {
            logger_.info(lastTickMillis_, line);
        }
    }

    if (mutating && response.status < 400) {
        // Anything that mutates state may have changed what should be on screen.
        scheduler_.invalidate();
    }
    return response;
}

}  // namespace host
}  // namespace stipple
