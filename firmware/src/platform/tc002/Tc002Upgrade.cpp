// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/platform/tc002/Tc002Upgrade.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>

namespace stipple {
namespace platform {
namespace tc002 {
namespace {

std::size_t sizeOf(const char* path) {
    struct stat info;
    if (::stat(path, &info) != 0) {
        return 0;
    }
    return static_cast<std::size_t>(info.st_size);
}

/// Write the whole buffer, then force it to the medium.
///
/// The fsync is the point. /data is jffs2 on raw flash, and the failure this
/// guards against is not a crash - it is the power cycle somebody performs
/// *because* they just updated the firmware and are waiting for it to come
/// back. Without this, the rename can land before the contents do.
bool writeWhole(const char* path, std::string_view data, std::string& problem) {
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        problem = "could not open a file to write to";
        return false;
    }

    std::size_t written = 0;
    while (written < data.size()) {
        const ssize_t chunk = ::write(fd, data.data() + written, data.size() - written);
        if (chunk <= 0) {
            ::close(fd);
            ::unlink(path);
            problem = "the write failed part-way; nothing has been changed";
            return false;
        }
        written += static_cast<std::size_t>(chunk);
    }

    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(path);
        problem = "the device would not confirm the write";
        return false;
    }

    ::close(fd);
    return true;
}

}  // namespace

std::size_t Tc002Upgrade::installedBytes() const { return sizeOf(kApplicationPath); }

bool Tc002Upgrade::hasPrevious() const { return sizeOf(kPreviousPath) > 0; }

bool Tc002Upgrade::install(std::string_view image, std::string& problem) {
    problem.clear();

    if (image.empty()) {
        problem = "nothing to install";
        return false;
    }

    // Written beside the target rather than over it. Until the rename at the
    // end, the running application and the one the shim would load next boot
    // are both untouched - so every failure below is a no-op rather than a
    // device that comes back to nothing.
    if (!writeWhole(kIncomingPath, image, problem)) {
        return false;
    }

    // Keep whatever is being displaced. Only meaningful from the second
    // install onwards: the first one displaces nothing, because the device is
    // running the copy flashed into /res, which cannot be lost.
    if (sizeOf(kApplicationPath) > 0) {
        ::unlink(kPreviousPath);
        if (::rename(kApplicationPath, kPreviousPath) != 0) {
            ::unlink(kIncomingPath);
            problem = "could not set the current version aside";
            return false;
        }
    }

    if (::rename(kIncomingPath, kApplicationPath) != 0) {
        // Put back what was moved, so a failure here is not the one case that
        // leaves the device with no override at all.
        ::rename(kPreviousPath, kApplicationPath);
        ::unlink(kIncomingPath);
        problem = "could not put the new version in place";
        return false;
    }

    // Only now. Every path above leaves the running application in place, so
    // reporting a pending restart before this point would be reporting one
    // that is not coming.
    restartPending_ = true;
    return true;
}

bool Tc002Upgrade::rollback(std::string& problem) {
    problem.clear();

    if (!hasPrevious()) {
        // Not an error worth dressing up: the device is running either its
        // first install or the copy flashed with the shim, and in both cases
        // there is nothing behind it.
        problem = "there is no previous version to go back to";
        return false;
    }

    if (::rename(kPreviousPath, kApplicationPath) != 0) {
        problem = "could not put the previous version back";
        return false;
    }

    // A rollback changes what loads next just as much as an install does.
    restartPending_ = true;
    return true;
}

}  // namespace tc002
}  // namespace platform
}  // namespace stipple
