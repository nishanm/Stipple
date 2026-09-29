// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <string_view>

#include "stipple/platform/Storage.h"

namespace stipple {
namespace config {

/// Bumped whenever the stored shape changes. Every bump needs migration code
/// and a test that loads a document written by the previous version.
///
/// v1 -> v2: display brightness moved from 0-100 percent to 0-255, matching the
/// panel's actual range instead of forcing a conversion at every use.
inline constexpr int kCurrentSchemaVersion = 2;

/// Dimming on a schedule.
///
/// The one display setting a clock on a bedside table actually needs. A panel
/// bright enough to read across a kitchen is far too bright to sleep next to,
/// and the alternative people reach for is unplugging it - which is also how
/// they stop using it.
///
/// Times are local minutes after midnight, so the window follows the timezone
/// rather than UTC. Storing an hour and a minute separately would invite two
/// fields to disagree; storing a timestamp would need a date this has no
/// business knowing.
struct NightSettings {
    bool enabled = false;

    /// 0-1439. A window that wraps midnight is the normal case rather than an
    /// edge case, so `start` greater than `end` is expected, not an error.
    int startMinutes = 22 * 60;
    int endMinutes = 7 * 60;

    /// What the panel dims to. Zero is allowed and means off, which is a
    /// legitimate thing to want overnight - and distinct from `enabled:false`,
    /// which means the schedule does not apply at all.
    std::uint8_t brightness = 16;
};

struct DisplaySettings {
    std::uint8_t brightness = 128;

    /// Overnight dimming. Overrides the brightness above while it applies,
    /// without overwriting it - so the morning gets the panel back exactly as
    /// the user left it rather than at whatever the night value was.
    NightSettings night;

    /// Panel on or off, without cutting power. Off still renders and presents —
    /// a black frame, so the panel is genuinely dark rather than holding the
    /// last image.
    bool power = true;

    /// Weather drawn over whatever app is showing, by name (see
    /// render::overlayFromName). A string for the same reason the clock face
    /// is one: an enum ordinal in a config file is unreadable, and
    /// renumbering it silently changes what a device does.
    std::string overlay = "none";
};

// Note: there is deliberately no `autoBrightness`. It was here, copied from what
// pixel-clock settings pages usually offer, until a third-party TC002 port
// confirmed the hardware has no ambient light sensor at all — see
// docs/research/tc002-platform-findings.md. A switch nothing can honour is
// exactly the kind of quietly-lying control this project refuses to ship
// elsewhere. If a future device does have a sensor, the honest shape is an
// optional platform capability that reports its presence (ADR 0013), not a
// config flag that hopes.

struct AudioSettings {
    /// 0-100. Stored as a percentage because that is how a volume control reads
    /// to a person; the conversion to the hardware's 0-255 happens once, at the
    /// edge, via volumeToByte.
    std::uint8_t volumePercent = 50;
};

/// 0-100 to 0-255, rounded rather than truncated so that 100% is exactly 255 and
/// a round trip through the settings API does not drift downwards.
constexpr std::uint8_t volumeToByte(std::uint8_t percent) noexcept {
    const unsigned clamped = percent > 100u ? 100u : percent;
    return static_cast<std::uint8_t>((clamped * 255u + 50u) / 100u);
}

struct MqttSettings {
    /// Off by default. MQTT is optional in both directions (§20): a device must
    /// be fully usable without it, and must never dial out to a broker nobody
    /// asked it to talk to.
    bool enabled = false;

    std::string host;
    int port = 1883;

    /// Empty means "derive from the device name", which is what makes a
    /// factory-fresh device work without anyone inventing an identifier.
    std::string clientId;
    std::string baseTopic = "stipple";

    std::string username;

    /// Never returned by the API and never written to the log (§22). It is
    /// stored because the device has to reconnect unattended; that is the only
    /// reason, and the only place it may appear.
    std::string password;

    /// Requested, not guaranteed — an adapter that cannot do TLS must refuse to
    /// connect rather than quietly send these credentials in the clear.
    bool tls = false;

    int keepAliveSeconds = 30;

    /// Publish Home Assistant discovery documents on connect.
    bool discovery = false;
};

/// What the user decided about one app, remembered across reboots.
///
/// Deliberately not the app itself. Built-in apps are recreated on every boot
/// and any app an integration pushed is gone with the power, so storing their
/// content here would be storing a copy that goes stale. What does need to
/// survive is the part the *user* chose: where it sits in the rotation, whether
/// it is in the rotation at all, and how long it stays up.
struct AppPreference {
    std::string id;
    bool enabled = true;

    /// Seconds on screen. Zero means "use the carousel default", matching
    /// App::durationSeconds.
    int durationSeconds = 0;
};

/// As many apps as the registry can hold. Kept here rather than including
/// AppRegistry.h, which would make configuration depend on the app layer for
/// one number; the assertion that they agree lives in the tests.
inline constexpr int kMaxRememberedApps = 32;

struct AppSettings {
    int defaultDurationSeconds = 8;

    /// Whether the carousel advances on its own. When false the panel holds
    /// whatever app is showing until someone moves it with the button or the
    /// API - for a display that should sit on one screen rather than rotate.
    bool autoAdvance = true;

    /// Whether app changes animate at all. Kept as the master switch because
    /// "off" is a thing people want for its own sake - a panel in a bedroom
    /// that simply changes is less distracting than any animation, however
    /// tasteful.
    bool transitions = true;

    /// Which animation, when they do. By name (see
    /// render::transitionStyleFromName), for the same reason the clock face
    /// and the overlay are: an enum ordinal in a config file is unreadable,
    /// and renumbering it silently changes what a device does.
    ///
    /// Applies to app-to-app changes only. A notification always fades in,
    /// because a slide would imply it is simply the next item in the rotation
    /// (DESIGN.md §6) - that is a statement about what a notification *is*,
    /// not a preference.
    std::string transition = "slide";

    /// Display order, first to last.
    ///
    /// Blueprint §12 is explicit that the app manager owns ordering and it must
    /// never be inferred - so an order the user arranged has to be written down
    /// or it is not really theirs. On load this is applied to whatever apps
    /// actually exist: an id that no longer exists is skipped, and an app that
    /// is not listed keeps its natural position at the end. Neither is an
    /// error, because both are normal - apps come and go, and a stored order
    /// from an older firmware should not stop a newer one booting.
    ///
    /// Bounded by AppRegistry::kMaxApps, like everything else about apps.
    std::vector<AppPreference> order;
};

struct ClockSettings {
    bool twentyFourHour = true;

    /// Fallback offset, used when `timezone` is empty or unparseable.
    ///
    /// Kept rather than replaced: a fixed offset is the right answer for the
    /// large part of the world that does not observe daylight saving, and it
    /// is what every device configured before timezones existed already has
    /// stored. Nothing should have to be re-entered to keep working.
    int utcOffsetSeconds = 0;

    /// Where to ask what time it is. A hostname or a dotted quad; empty
    /// turns synchronisation off.
    ///
    /// This device has no RTC battery, so it boots at 1970 every time and
    /// this is the only thing between a cold boot and a clock that shows
    /// `__:__` for ever. Configurable because a network that blocks outbound
    /// NTP, or one with its own server, is ordinary rather than exotic.
    /// Numeric on purpose, and this is not a stylistic choice.
    ///
    /// DNS does not work on a stock TC002: the resolver reads only
    /// /etc/resolv.conf, that file is on the read-only rootfs, and it lists
    /// 114.114.114.114 first - a China-only service that does not answer from
    /// elsewhere, so getaddrinfo stalls instead of failing over. Measured:
    /// resolving pool.ntp.org never returned and the clock stayed at 1970;
    /// the same code pointed at this address set it in under eight seconds.
    ///
    /// 162.159.200.1 is time.cloudflare.com's anycast address, so it is close
    /// to wherever the device is. A hostname still works if DNS does, and
    /// empty turns synchronisation off entirely.
    std::string ntpServer = "162.159.200.1";

    /// A POSIX TZ rule, e.g. "CET-1CEST,M3.5.0,M10.5.0/3".
    ///
    /// When set and parseable this wins, and the clock follows daylight saving
    /// on its own. Empty means "use the offset above", which is what an
    /// unconfigured device and every older one does.
    ///
    /// A rule rather than a zone name because there is no timezone database on
    /// this hardware - no /usr/share/zoneinfo, no /etc/localtime - and shipping
    /// one would cost megabytes on an 8 MiB partition and go stale the moment a
    /// government moved a date. See stipple::timezone_::Timezone.
    std::string timezone;

    /// Clock face, by name (see apps::clockThemeFromName). Stored as a string
    /// rather than an enum so that configuration does not depend on the app
    /// layer, and so an unrecognised value from a newer build degrades to the
    /// default instead of failing the whole load.
    ///
    /// Adding this needed no schema bump: an absent field takes its default, so
    /// a v2 document still loads unchanged.
    std::string theme = "minimal";

    /// Show 07:05 rather than 7:05.
    bool leadingZero = true;

    /// 12-hour clock only, and only on faces with room for it.
    bool showAmPm = false;

    /// Colours as packed 0xRRGGBB. Packed rather than `Rgb` so that config stays
    /// a plain serialisable aggregate, and stored as integers rather than
    /// strings so a malformed colour cannot appear mid-load; the text form is a
    /// presentation detail of the API and the stored document.
    std::uint32_t color = 0xFFFFFFu;
    std::uint32_t accentColor = 0x00BEFFu;
    std::uint32_t dateColor = 0x00BEFFu;

    /// Date field order, separator and year width, by name. Same reasoning as
    /// `theme`: an unrecognised value from a newer build degrades to the default
    /// rather than failing the load.
    /// These must be spelled exactly as apps::dateOrderName and friends return
    /// them: the settings API accepts only names that survive a name -> enum ->
    /// name round trip, so a default that did not round-trip would be rejected
    /// by the very endpoint that reports it.
    std::string dateOrder = "dayMonthYear";
    std::string dateSeparator = "dot";
    std::string dateYear = "none";

    /// Colon blink period in milliseconds; 0 holds it lit.
    std::uint32_t blinkPeriodMillis = 1000;

    /// An audible tick, once a second, while the clock is on screen.
    ///
    /// Off by default, and that is not timidity: a sound a device makes once a
    /// second without being asked is the single easiest way to make somebody
    /// unplug it. Someone who wants a ticking clock will go and find this;
    /// nobody who does not want one should have to.
    ///
    /// Silently ignored where there is no speaker, like every other sound.
    bool tick = false;
};

/// The audio visualiser's own settings. The first app to have any, and the
/// shape the rest should follow: one struct per app, named for the app, rather
/// than a flat pile of fields on Config.
/// Sounds the device makes on its own behalf.
struct NotificationSettings {
    /// Played when a notification appears and does not name its own sound.
    /// "none" keeps the device silent, which is what a clock in a bedroom
    /// wants. By name, for the same reason every other choice here is.
    std::string sound = "chime";
};

struct VisualizerSettings {
    /// "meter" or "trace", by name for the same reason the clock face is: an
    /// enum ordinal in a config file is unreadable, and renumbering it silently
    /// changes what a device does. An unrecognised value falls back to the
    /// default rather than failing the load.
    std::string style = "meter";
};

/// The glucose app's data source and face.
///
/// No default for the URL and none for the credential: this repository is
/// public, and a LAN address or a secret compiled in would be published with
/// it. Both arrive through the API and live only on the device.
struct GlucoseSettings {
    /// Nightscout base URL, `http://host:port`, no trailing slash. Empty means
    /// the app has no source and shows its no-data face.
    std::string url;

    /// The SHA-1 of the site's API secret, as lowercase hex - the form the
    /// `api-secret` header carries. **The plaintext is never stored**: the API
    /// hashes what it is given and keeps only this. Write-only like the MQTT
    /// password; the API reports whether one is set and nothing else.
    std::string apiSecretSha1;

    /// Seconds between fetches. Clamped to 30..600 on load and on write.
    int pollSeconds = 60;

    /// Which face to show while a reading is fresh, by name (GlucoseFace).
    /// A stale reading shows the no-data face whatever this says.
    std::string face = "hero";
};

/// Who may talk to this device (ADR 0018).
///
/// HTTP Basic, one mechanism for the API and the browser, because they are the
/// same server and two schemes would mean two things to get wrong. A browser
/// prompts for it natively and every automation library already speaks it.
///
/// Off by default: a device that demanded a password before it would show a
/// clock would be a worse first five minutes than the risk it removes.
struct WebSettings {
    /// Both empty means no authentication. Requiring one and not the other is
    /// refused at the API rather than half-applied.
    std::string username;

    /// **Never returned by the API**, exactly like the MQTT password - which
    /// also keeps it out of backups, since those are taken from the API. A
    /// settings file in somebody's downloads folder should not be a credential.
    ///
    /// Stored on the device in the clear, and worth stating plainly rather than
    /// implying otherwise. Anyone with ADB can read it, but anyone with ADB can
    /// already replace the firmware; what this defends against is the rest of
    /// the LAN. Hashing it is a later increment, not a reason to ship nothing.
    std::string password;
};

/// How the device gets onto a network (ADR 0018).
struct NetworkSettings {
    /// Set by the rescue gesture and by a device that has nowhere to go, and
    /// cleared once a real network is joined. Stored rather than held in memory
    /// so a rescue survives the reboot somebody reaches for next.
    bool hotspotRequested = false;
};

struct Config {
    int schemaVersion = kCurrentSchemaVersion;
    std::string deviceName = "stipple";
    DisplaySettings display;
    AudioSettings audio;
    MqttSettings mqtt;
    AppSettings apps;
    ClockSettings clock;
    WebSettings web;
    NetworkSettings network;
    NotificationSettings notifications;
    VisualizerSettings visualizer;
    GlucoseSettings glucose;
};

enum class LoadStatus : std::uint8_t {
    Loaded,               ///< primary read cleanly at the current schema
    Migrated,             ///< primary read cleanly and was upgraded
    RecoveredFromBackup,  ///< primary unusable, backup was good
    DefaultsMissing,      ///< nothing stored yet; first boot
    DefaultsCorrupt,      ///< stored data failed checksum or parsing
    DefaultsFutureSchema, ///< written by newer firmware; refused rather than guessed
};

const char* describe(LoadStatus status) noexcept;

struct LoadReport {
    LoadStatus status = LoadStatus::DefaultsMissing;
    int fromSchemaVersion = 0;
    bool usedBackup = false;
};

/// Persistent configuration with recovery (blueprint §21).
///
/// Stored as `{"checksum":"...","body":{...}}`, where the checksum covers the
/// body text exactly as written. Layout and behaviour follow from one rule: a
/// malformed or truncated configuration must never brick the clock or cause a
/// boot loop. So every failure path ends at usable settings — backup first,
/// then defaults — and the caller is told which happened rather than having to
/// guess from behaviour.
///
/// Unknown fields are ignored, so a config written by a newer *minor* build
/// still loads. A newer *schema* is refused outright: misinterpreting fields
/// whose meaning has changed is worse than starting from defaults.
class ConfigStore {
public:
    static constexpr std::string_view kPrimaryKey = "config";
    static constexpr std::string_view kBackupKey = "config.bak";
    /// Token budget for parsing a stored document. Must stay comfortably ahead
    /// of what serialize() produces: overflowing it makes a perfectly good
    /// configuration read as corrupt, and the device would silently fall back to
    /// defaults. Config.TokenBudgetHasHeadroom asserts the margin.
    ///
    /// Raised from 256 when app order arrived. Each remembered app is about
    /// seven tokens - an object, three keys and three values - so a full
    /// registry of 32 costs more than the entire rest of the document. At 256
    /// a device with a lot of apps would have read its own perfectly good
    /// configuration as corrupt and quietly reset itself, which is the exact
    /// failure this constant exists to prevent.
    ///
    /// The cost is stack: the parser holds this many 12-byte tokens in one
    /// frame, so 1024 is 12 KB. That is affordable here and worth measuring
    /// again before it grows much further.
    static constexpr int kMaxTokens = 1024;

    explicit ConfigStore(platform::IStorage& storage) noexcept : storage_(storage) {}

    /// Always leaves `out` usable, whatever the stored data looks like.
    LoadReport load(Config& out) const;

    /// Copies the current value to the backup key before replacing the primary,
    /// so an interrupted save leaves the previous configuration recoverable.
    bool save(const Config& config);

    static std::string serialize(const Config& config);

    /// Returns false if the payload is unparseable, fails its checksum, or
    /// carries a newer schema — in which case `futureSchema` is set.
    static bool deserialize(std::string_view payload,
                            Config& out,
                            int& fromSchemaVersion,
                            bool& futureSchema);

private:
    platform::IStorage& storage_;
};

}  // namespace config
}  // namespace stipple
