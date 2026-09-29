// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stipple {
namespace app {

/// Where an app came from (blueprint §12). Affects lifetime and who may modify
/// it, not how it renders.
enum class AppSource : std::uint8_t {
    System,       ///< built in; cannot be deleted
    Local,        ///< created on the device
    Remote,       ///< pushed over the API
    Integration,  ///< owned by Home Assistant, Domoticz, Node-RED, ...
    Temporary,    ///< disappears when its lifetime elapses
};

const char* appSourceName(AppSource source) noexcept;

/// Apps whose content is code rather than a scene document.
///
/// A built-in exists where a scene cannot express the content — the clock needs
/// live time formatting, the bring-up pattern needs per-frame animation. Kept as
/// an explicit tag rather than matching on well-known ids, so the special case is
/// visible in the type instead of hidden in a string comparison.
enum class Builtin : std::uint8_t {
    None,         ///< renders `sceneJson`
    Clock,
    Battery,
    Visualizer,
    Stopwatch,
    TestPattern,
    /// A glucose reading drawn in one of the glucose faces. Its content comes
    /// from a data source rather than a scene, and its hero digit is taller
    /// than the text engine allows, so it is code.
    Glucose,
    /// Content is a Berry script, held in the ScriptStore under this app's id.
    ///
    /// The source is not in the App because an App is copied whenever the
    /// carousel is read, and copying a script's source on every frame would
    /// be absurd. The id is the link, and the store owns the text and the
    /// interpreter running it.
    Script,
};

/// One entry in the carousel.
///
/// The scene is held as JSON text rather than as a parsed structure. Parsed
/// token storage costs several kilobytes per app, which would not survive
/// thirty apps on a device with this little RAM; instead a single shared token
/// buffer is reused for whichever app is currently on screen.
struct App {
    std::string id;
    std::string name;
    std::string sceneJson;

    /// Seconds on screen. Zero means "use the carousel default".
    int durationSeconds = 0;

    bool enabled = true;
    AppSource source = AppSource::Local;
    Builtin builtin = Builtin::None;

    /// Monotonic deadline for Temporary apps. Zero means it never expires.
    std::uint64_t expiresAtMillis = 0;

    bool expires() const noexcept { return expiresAtMillis != 0; }
};

/// Ordered collection of apps.
///
/// Blueprint §12 is explicit: the app manager owns ordering, and it must never
/// be inferred from filesystem enumeration or associative-container iteration.
/// So the store is a vector in explicit display order and lookup by id is a
/// linear scan. With a cap of 32 apps that scan costs nothing, and it removes
/// any chance of the carousel silently reordering itself because a hash changed.
class AppRegistry {
public:
    static constexpr int kMaxApps = 32;
    static constexpr std::size_t kMaxSceneBytes = 4096;
    static constexpr std::size_t kMaxIdBytes = 64;

    enum class PutResult {
        Added,
        Replaced,
        Full,
        InvalidId,
        SceneTooLarge,
    };

    /// Insert, or replace an existing app with the same id.
    ///
    /// A replacement keeps its position: updating an app's content from an
    /// integration must not shuffle the carousel under the user.
    PutResult put(App app);

    bool remove(std::string_view id);

    /// Removes everything except System apps, which exist to guarantee the
    /// device still shows something after a bad API call.
    void clear();

    int count() const noexcept { return static_cast<int>(apps_.size()); }
    bool empty() const noexcept { return apps_.empty(); }

    const App* at(int index) const noexcept;
    App* at(int index) noexcept;

    const App* find(std::string_view id) const noexcept;
    App* find(std::string_view id) noexcept;

    /// Position in display order, or -1.
    int indexOf(std::string_view id) const noexcept;

    /// Move an app to an explicit position, shifting the others along.
    bool move(std::string_view id, int newIndex);

    bool setEnabled(std::string_view id, bool enabled);

    int enabledCount() const noexcept;

    /// Index of the next or previous enabled app, wrapping. Returns -1 when
    /// nothing is enabled.
    int nextEnabled(int fromIndex) const noexcept;
    int previousEnabled(int fromIndex) const noexcept;
    int firstEnabled() const noexcept;

    /// Drop Temporary apps whose deadline has passed. Returns how many went.
    int expire(std::uint64_t nowMillis);

    /// Bumped on every mutation. Anything caching a view into an app's scene
    /// JSON watches this to know when that view may have been invalidated.
    std::uint32_t revision() const noexcept { return revision_; }

private:
    std::vector<App> apps_;
    std::uint32_t revision_ = 0;
};

}  // namespace app
}  // namespace stipple
