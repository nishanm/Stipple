# SPDX-License-Identifier: GPL-3.0-or-later
#
# STIPPLE developer entry point (blueprint §29.2).
#
#   .\dev.ps1 build          configure + build the host target
#   .\dev.ps1 test           build and run the host test suite
#   .\dev.ps1 test Canvas    run only tests whose name contains "Canvas"
#   .\dev.ps1 golden         rebuild golden-image fixtures (review the PNGs!)
#   .\dev.ps1 preview        render frames to PNG and open them (no EMSDK needed)
#   .\dev.ps1 ci             what CI runs: warnings as errors, strict goldens
#   .\dev.ps1 emulator       build the browser emulator (needs EMSDK)
#   .\dev.ps1 verify         drive the built WASM module headlessly (needs node)
#   .\dev.ps1 device         cross-build for the TC002 and run it under ARM emulation
#   .\dev.ps1 deploy <ip>    build STIPPLE and run it on the device
#   .\dev.ps1 panel <ip>     build the channel-order test and run it on the panel
#   .\dev.ps1 serve          build the emulator and serve it on localhost
#   .\dev.ps1 clean          remove build output
#   .\dev.ps1 doctor         report toolchain status
#
# 'deploy' is ADR 0008's tier 2: volatile, /tmp, stock app restored by a power
# cycle. It uses the *bullseye* toolchain because stipple_device is dynamically
# linked for the speaker, and a Debian 12 binary demands a glibc this device
# does not have. 'device' still uses bookworm, because the smoke test it builds
# is static and does not care.
#
# **Neither of these is the compiler CI's ARMv7 job uses, and that gap has
# already cost one red release.** That job installs crossbuild-essential-armhf
# on the runner, so it is whatever ubuntu-latest ships - GCC 13 at the time of
# writing, against bullseye's 10 and bookworm's 12. Newer libstdc++ headers
# prune transitive includes, so a translation unit that forgot <cstdint> and
# used std::uint32_t compiled clean in both containers and failed in CI.
#
# To check against CI's compiler before pushing:
#
#   podman run --rm -v "${PWD}:/src" ubuntu:24.04 bash -c '
#     apt-get update -qq && apt-get install -y -qq crossbuild-essential-armhf ninja-build cmake
#     cd /src && cmake -S . -B /tmp/arm -G Ninja #       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-linux-gnueabihf.cmake #       -DSTIPPLE_DEVICE_BUILD=ON -DSTIPPLE_BUILD_TESTS=OFF #       -DSTIPPLE_WARNINGS_AS_ERRORS=ON && cmake --build /tmp/arm'

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('build', 'test', 'ci', 'golden', 'preview', 'emulator', 'verify', 'device', 'deploy', 'capture', 'image', 'usb', 'panel', 'serve', 'site', 'previews', 'release', 'clean', 'doctor')]
    [string]$Command = 'build',

    [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot

function Get-VsInstallPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    return & $vswhere -latest -products * -property installationPath
}

# Ninja invokes link.exe directly and inherits LIB/INCLUDE/PATH from the shell.
# A plain PowerShell prompt has no developer environment, and what CMake falls
# back to targets x86 while the compiler it picks is x64 — every CRT symbol then
# resolves against the wrong architecture and the link fails with LNK4272 plus a
# wall of unresolved externals. Entering the x64 dev shell first keeps the
# toolchain self-consistent.
function Initialize-MsvcEnvironment {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64') { return }

    $vsPath = Get-VsInstallPath
    if (-not $vsPath) { return }

    $devShell = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path $devShell)) { return }

    Import-Module $devShell -ErrorAction Stop
    # VsDevCmd.bat prints a harmless "vswhere.exe is not recognized" line from a
    # child cmd process, straight to the console handle — PowerShell redirection
    # cannot suppress it. Ignore the noise and check the outcome below instead.
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation `
        -DevCmdArguments '-arch=x64 -host_arch=x64' *>&1 | Out-Null

    if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
        throw "could not enter an x64 Visual Studio developer environment"
    }
}

function Find-CMake {
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    # Visual Studio ships CMake but does not put it on PATH.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -property installationPath
        if ($vsPath) {
            $candidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }

    throw "cmake not found. Install the 'Desktop development with C++' workload in Visual Studio, or run: winget install Kitware.CMake"
}

function Find-CTest {
    $cmake = Find-CMake
    $candidate = Join-Path (Split-Path $cmake -Parent) 'ctest.exe'
    if (Test-Path $candidate) { return $candidate }
    $onPath = Get-Command ctest -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw "ctest not found next to cmake at $cmake"
}

function Invoke-Configure([string]$Preset) {
    Initialize-MsvcEnvironment
    $cmake = Find-CMake
    & $cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for preset '$Preset'" }
}

function Invoke-Build([string]$Preset) {
    Initialize-MsvcEnvironment
    $cmake = Find-CMake
    if (-not (Test-Path (Join-Path $repoRoot "build\$Preset\CMakeCache.txt"))) {
        Invoke-Configure $Preset
    }
    & $cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "build failed for preset '$Preset'" }
}

function Get-TestBinary([string]$Preset = 'host-debug') {
    # Single-config generators emit into the preset directory; multi-config ones
    # nest under the configuration name. Switching generators leaves the old
    # binary behind, and running a stale one silently reports stale results, so
    # pick the most recently written rather than the first path that exists.
    $testDir = Join-Path $repoRoot "build\$Preset\firmware\tests"
    if (-not (Test-Path $testDir)) {
        throw "no test output at $testDir — did the build succeed?"
    }

    $binary = Get-ChildItem $testDir -Recurse -Filter "stipple_tests.exe" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $binary) { throw "test binary not found under $testDir — did the build succeed?" }
    return $binary.FullName
}

switch ($Command) {
    'build' {
        Invoke-Build 'host-debug'
        Write-Host "`nBuild complete." -ForegroundColor Green
    }

    'test' {
        Invoke-Build 'host-debug'
        $binary = Get-TestBinary
        $filter = if ($Rest) { $Rest[0] } else { '' }
        & $binary $filter
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    'ci' {
        # What CI actually runs: warnings as errors, and missing golden fixtures
        # treated as failures rather than quietly created. Worth having locally,
        # because the alternative is finding out from a red build.
        Invoke-Build 'ci'
        $binary = Get-TestBinary 'ci'
        $env:STIPPLE_STRICT_GOLDEN = '1'
        try { & $binary } finally { Remove-Item env:STIPPLE_STRICT_GOLDEN -ErrorAction SilentlyContinue }
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Host "`nCI checks passed." -ForegroundColor Green
    }

    'verify' {
        # Drives the built WASM module headlessly. The C++ suite cannot catch a
        # missing Emscripten export or a stale EXPORTED_FUNCTIONS list; this can.
        $node = Get-Command node -ErrorAction SilentlyContinue
        if (-not $node) { throw "node not found on PATH; needed to verify the emulator build" }

        $harness = Join-Path $repoRoot 'simulator/web/tools/verify.mjs'
        & $node.Source $harness
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    'device' {
        # Cross-compiles for the TC002 and runs the result under ARM emulation.
        # Needs no hardware, and answers the questions that are expensive to get
        # wrong on a device: does the core build for ARMv7, does it link, does it
        # execute, and what does the binary depend on.
        $engine = (Get-Command podman -ErrorAction SilentlyContinue) ??
                  (Get-Command docker -ErrorAction SilentlyContinue)
        if (-not $engine) {
            throw "podman or docker is needed to run the pinned cross-toolchain. See tooling/cross/."
        }

        $image = 'stipple-cross:bookworm'
        & $engine.Source build -t $image -f tooling/cross/Containerfile tooling/cross
        if ($LASTEXITCODE -ne 0) { throw "could not build the cross-toolchain image" }

        $script = @'
set -e
cmake --preset device-arm
cmake --build --preset device-arm
cd /src/build/device-arm/firmware
echo
echo "--- artifact ---"
file stipple_device_smoke
arm-linux-gnueabihf-strip -o /tmp/stripped stipple_device_smoke
echo "stripped: $(stat -c %s /tmp/stripped) bytes"
readelf -d stipple_device_smoke | grep NEEDED || echo "shared libraries: none (static)"
echo
echo "--- running on ARM ---"
qemu-arm-static stipple_device_smoke
'@
        # PowerShell here-strings carry CRLF line endings, and bash reads the
        # carriage return as part of each command, so every path ends in an
        # invisible character and nothing resolves.
        $script = $script -replace "`r", ""

        & $engine.Source run --rm -v "${repoRoot}:/src" $image bash -c $script
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Host "`nDevice build runs on ARM." -ForegroundColor Green
    }

    'deploy' {
        # Tier 2 from ADR 0008: build STIPPLE, push it to /tmp, run it. Volatile
        # by construction - /tmp is tmpfs, so a power cycle restores the stock
        # application whatever happens here.
        #
        # **Bullseye, not bookworm, and that is the whole point of this verb.**
        #
        # stipple_device is dynamically linked, because the speaker is only
        # reachable through /lib/libmi_ao.so and a static binary cannot dlopen.
        # A Debian 12 toolchain emits executables needing GLIBC_2.34 for
        # __libc_start_main; this device carries 2.30, so such a binary does not
        # start and says so by naming a symbol rather than the cause. Building
        # it by hand in the wrong image is a mistake that costs half an hour,
        # and it is exactly the mistake this verb exists to stop anyone making
        # twice.
        #
        # Presets are not used here: CMakePresets.json requires CMake 3.21 and
        # bullseye ships 3.18, which is also why cmake_minimum_required is 3.18.
        # The flags below are the device-arm preset, spelled out.
        $target = if ($Rest) { $Rest[0] } else { $null }

        $adb = Get-Command adb -ErrorAction SilentlyContinue
        if (-not $adb) { throw "adb not found on PATH. See docs/bring-up.md for how to get it." }

        $engine = (Get-Command podman -ErrorAction SilentlyContinue) ??
                  (Get-Command docker -ErrorAction SilentlyContinue)
        if (-not $engine) {
            throw "podman or docker is needed to run the pinned cross-toolchain. See tooling/cross/."
        }

        $image = 'stipple-cross:bullseye'
        & $engine.Source build -t $image -f tooling/cross/Containerfile.bullseye tooling/cross
        if ($LASTEXITCODE -ne 0) { throw "could not build the bullseye cross-toolchain image" }

        $script = @'
set -e
cmake -S /src -B /src/build/device-arm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/src/cmake/toolchains/arm-linux-gnueabihf.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DSTIPPLE_BUILD_TESTS=OFF \
  -DSTIPPLE_DEVICE_BUILD=ON \
  -DSTIPPLE_WARNINGS_AS_ERRORS=ON
cmake --build /src/build/device-arm --target stipple_device

echo
echo "--- artifact ---"
file /src/build/device-arm/firmware/stipple_device

# The check that would have caught the wrong image. The device carries glibc
# 2.30 and GLIBCXX 3.4.28; anything above either will not start.
echo "--- highest versioned symbols required ---"
arm-linux-gnueabihf-readelf -V /src/build/device-arm/firmware/stipple_device \
  | grep -oE 'GLIBC_[0-9.]+|GLIBCXX_[0-9.]+' | sort -u -V | tail -4
'@
        $script = $script -replace "`r", ""

        & $engine.Source run --rm -v "${repoRoot}:/src" $image bash -c $script
        if ($LASTEXITCODE -ne 0) { throw "cross-build failed" }

        $binary = Join-Path $repoRoot 'build\device-arm\firmware\stipple_device'
        if (-not (Test-Path $binary)) { throw "expected $binary after the build" }

        if ($target) {
            & $adb.Source connect $target | Out-Null
        }

        # Anything already running owns the panel and port 80, so it has to go
        # first - otherwise the new process reports "port in use" and renders
        # nothing, which looks like a build problem and is not.
        $running = & $adb.Source shell ps 2>&1 | Select-String 'stipple_device'
        foreach ($line in $running) {
            $id = $line.Line.Trim().Split(' ')[0]
            & $adb.Source shell "kill -9 $id" | Out-Null
        }

        & $adb.Source push $binary /tmp/stipple_device
        if ($LASTEXITCODE -ne 0) { throw "adb push failed - is the device connected?" }
        & $adb.Source shell chmod 700 /tmp/stipple_device

        # The vendor application drives the same panel and neither arbitrates.
        & $adb.Source shell "setprop ctl.stop zkswe" | Out-Null
        Start-Sleep -Seconds 2

        Write-Host "`nSTIPPLE is running. Ctrl-C here stops it." -ForegroundColor Green
        Write-Host "  Restore the stock clock with: adb shell setprop ctl.start zkswe" -ForegroundColor Gray
        Write-Host ""

        & $adb.Source shell /tmp/stipple_device
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    'capture' {
        # ADR 0008's hard precondition, and the one verb that has to work
        # before any of the others are allowed to exist.
        #
        # It reads the live res partition through the kernel's read-only
        # alias and wraps it in a container the device's own loader will
        # accept. Nothing is written to the device.
        #
        # Worth knowing why this is not optional: every TC002 ships with an
        # update.img on its own USB volume, and holding reset installs it -
        # but on the unit this was written against that image is *older* than
        # the res partition actually running. The reset button is a downgrade
        # unless somebody has put the right image there first.
        $target = if ($Rest) { $Rest[0] } else { $null }

        $adb = Get-Command adb -ErrorAction SilentlyContinue
        if (-not $adb) { throw "adb not found on PATH. See docs/bring-up.md." }

        if ($target) { & $adb.Source connect $target | Out-Null }

        $python = Get-Command python.exe -ErrorAction SilentlyContinue
        if (-not $python) { throw "python.exe not found on PATH." }

        $script = Join-Path $repoRoot 'tooling\imgtool\capture.py'
        $arguments = @($script, '--out', (Join-Path $repoRoot 'restore'))
        if ($target) { $arguments += @('--target', $target) }

        & $python.Source @arguments
        if ($LASTEXITCODE -ne 0) { throw "capture failed" }

        Write-Host "`nKeep restore/. It is specific to this device, it is Ulanzi's" -ForegroundColor Yellow
        Write-Host "firmware, and it is gitignored for both reasons." -ForegroundColor Yellow
    }

    'image' {
        # Build a res partition image from a capture.
        #
        # The image carries STIPPLE itself, not just a pointer at it. An
        # earlier design shipped only the shim and left STIPPLE in /data,
        # which works right up until /data holds an old copy or none: the
        # flash succeeds and the device runs last week's build, or the stock
        # clock, with nothing to say why. That happened twice on real
        # hardware and was read as a bad flash both times. See the note in
        # tooling/imgtool/buildres.sh.
        #
        # It runs in the container and not on the host because the res
        # filesystem stores uid/gid 1000 and modes like 0770, and extracting
        # it onto a Windows bind mount flattens both to root/0777 - which
        # would silently change the ownership of every file on the partition.
        $capture = Join-Path $repoRoot 'restore\res-raw.bin'
        if (-not (Test-Path $capture)) {
            throw "no capture at restore\res-raw.bin - run '.\dev.ps1 capture <target>' first"
        }

        $engine = (Get-Command podman -ErrorAction SilentlyContinue) ??
                  (Get-Command docker -ErrorAction SilentlyContinue)
        if (-not $engine) { throw "podman or docker is needed for the pinned toolchain." }

        $image = 'stipple-cross:bullseye'
        & $engine.Source build -t $image -f tooling/cross/Containerfile.bullseye tooling/cross | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "could not build the cross-toolchain image" }

        $script = 'bash /src/tooling/imgtool/buildres.sh /src/restore/res-raw.bin /src/restore/stipple-res.squashfs'
        & $engine.Source run --rm -v "${repoRoot}:/src" $image bash -c $script
        if ($LASTEXITCODE -ne 0) { throw "could not build the res image" }

        $python = Get-Command python.exe -ErrorAction SilentlyContinue
        if (-not $python) { throw "python.exe not found on PATH." }
        & $python.Source (Join-Path $repoRoot 'tooling\imgtool\imgtool.py') pack `
            (Join-Path $repoRoot 'restore\stipple-res.squashfs') `
            (Join-Path $repoRoot 'restore\stipple-update.img') `
            --template (Join-Path $repoRoot 'restore\shipped-update.img')
        if ($LASTEXITCODE -ne 0) { throw "could not wrap the image" }

        Write-Host "`nNothing has been flashed. ADR 0008 gates that on a" -ForegroundColor Yellow
        Write-Host "demonstrated restore, which has not happened." -ForegroundColor Yellow
    }

    'usb' {
        # Prepare a stick the device will flash from.
        #
        # Wrapped rather than reimplemented: the script carries the guards,
        # and this verb exists so nobody has to know that FAT32 needs 4 KB
        # clusters or that a one-byte file called zkautoupgrade is what makes
        # the loader look at external media at all.
        $script = Join-Path $repoRoot 'tooling\installer\Prepare-UsbStick.ps1'
        # A hashtable, not an array. Splatting an array passes its elements
        # positionally, so @('-Drive', 'D:') bound '-Drive' to the first
        # parameter and 'D:' to the second - which made 'dev.ps1 usb C:' try
        # to validate C: as a firmware image.
        $extra = @{}
        if ($Rest -and $Rest[0]) { $extra['Drive'] = $Rest[0] }
        & $script @extra
        # No throw on top: the script has already said what went wrong, in
        # more detail than a wrapper could, and a stack trace over it just
        # buries the explanation.
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    'previews' {
        # Render the shop's animated previews.
        #
        # Two steps because the frames come from the real engine and the GIF
        # is built from them: a preview drawn any other way would be a picture
        # of something that does not exist.
        $frames = Join-Path $repoRoot 'build\frames'
        $defaults = Join-Path $repoRoot 'build\frames-defaults'
        New-Item -ItemType Directory -Force $frames, $defaults | Out-Null

        Invoke-Build 'host-release'
        $env:STIPPLE_SHOP_FRAMES = $frames
        $env:STIPPLE_DEFAULT_FRAMES = $defaults
        try {
            & (Join-Path $repoRoot 'build\host-release\firmware\tests\stipple_tests.exe') `
                'ShopScripts.WriteFramesOnRequest'
            if ($LASTEXITCODE -ne 0) { throw 'the frame dump failed' }
            & (Join-Path $repoRoot 'build\host-release\firmware\tests\stipple_tests.exe') `
                'ShopScripts.WriteDefaultAppFramesOnRequest'
            if ($LASTEXITCODE -ne 0) { throw 'the default app frame dump failed' }
        } finally {
            Remove-Item Env:\STIPPLE_SHOP_FRAMES -ErrorAction SilentlyContinue
            Remove-Item Env:\STIPPLE_DEFAULT_FRAMES -ErrorAction SilentlyContinue
        }

        $python = Get-Command python.exe -ErrorAction SilentlyContinue
        if (-not $python) { throw 'python.exe not found on PATH.' }
        & $python.Source (Join-Path $repoRoot 'tooling\site\build-previews.py') $frames
        if ($LASTEXITCODE -ne 0) { throw 'could not build the previews' }
        & $python.Source (Join-Path $repoRoot 'tooling\site\build-previews.py') $defaults (Join-Path $repoRoot 'site\defaults')
        if ($LASTEXITCODE -ne 0) { throw 'could not build the default app previews' }
    }

    'release' {
        # Cut a release, or say exactly why it cannot be cut.
        #
        # The workflow does all of this by itself when run from the Actions
        # tab - this verb exists for the case where somebody wants the bump in
        # a pull request, or wants to know whether the notes are in order
        # before clicking anything.
        #
        #     .\dev.ps1 release            # what would happen
        #     .\dev.ps1 release 0.2.4      # write that version locally
        #
        # It never tags and never pushes. Tagging from a laptop is how a
        # release gets cut from a tree that has uncommitted changes in it.
        $asked = if ($Rest) { $Rest[0] } else { $null }

        $cmakeVersion = (Select-String -Path (Join-Path $repoRoot 'CMakeLists.txt') `
            -Pattern '^\s*VERSION\s+(\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
        $headerVersion = (Select-String -Path (Join-Path $repoRoot 'firmware\include\stipple\core\Version.h') `
            -Pattern 'kVersion\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value

        Write-Host "CMakeLists.txt  $cmakeVersion"
        Write-Host "Version.h       $headerVersion"
        if ($cmakeVersion -ne $headerVersion) {
            Write-Host "  they disagree - a release would be refused" -ForegroundColor Red
        }

        $tags = @(git tag -l 'v[0-9]*.[0-9]*.[0-9]*' | ForEach-Object { $_.Substring(1) })
        $latest = if ($tags) {
            ($tags | Sort-Object { [version]$_ } | Select-Object -Last 1)
        } else { '0.0.0' }
        Write-Host "latest tag      v$latest"

        $changelog = Get-Content (Join-Path $repoRoot 'CHANGELOG.md') -Raw
        $hasUnreleased = $changelog -match '(?m)^##\s+Unreleased'
        Write-Host ("notes           " + $(if ($hasUnreleased) { "'## Unreleased' is ready to be named" } else { "no '## Unreleased' section" }))

        if (-not $asked) {
            $parts = $latest.Split('.')
            $next = "$($parts[0]).$($parts[1]).$([int]$parts[2] + 1)"
            Write-Host ""
            Write-Host "Nothing changed. To cut a release:" -ForegroundColor Cyan
            Write-Host "  - write the notes under '## Unreleased' in CHANGELOG.md"
            Write-Host "  - merge to main"
            Write-Host "  - run the Release workflow from the Actions tab"
            Write-Host ""
            Write-Host "It picks the version itself: a patch bump would make it $next." -ForegroundColor Gray
            Write-Host "To write a version here instead: .\dev.ps1 release $next" -ForegroundColor Gray
            break
        }

        $bash = Get-Command bash -ErrorAction SilentlyContinue
        if (-not $bash) { throw "bash not found on PATH (Git for Windows provides it)" }

        & $bash.Source (Join-Path $repoRoot 'tooling/release/bump.sh') $asked
        if ($LASTEXITCODE -ne 0) { throw "bump failed" }

        Write-Host ""
        Write-Host "Written. Nothing has been tagged or pushed." -ForegroundColor Green
        Write-Host "Commit it, merge it, then run the Release workflow." -ForegroundColor Gray
    }

    'site' {
        # Preview the website. Regenerates the panel data first, so what you
        # look at is the frames the tests currently verify rather than
        # whatever was generated last time.
        $python = Get-Command python.exe -ErrorAction SilentlyContinue
        if (-not $python) { throw 'python.exe not found on PATH.' }
        & $python.Source (Join-Path $repoRoot 'tooling\site\build-frames.py')
        if ($LASTEXITCODE -ne 0) { throw 'could not build the panel data' }

        $node = Get-Command node -ErrorAction SilentlyContinue
        if ($node) { & $node.Source (Join-Path $repoRoot 'tooling\site\check-site.mjs') }

        # The listing and the reference pages, rendered from scripts/ and
        # docs/ - none of the three is committed, so without this the preview
        # is a site with three holes in it.
        & $python.Source (Join-Path $repoRoot 'tooling\site\build-shop.py')
        if ($LASTEXITCODE -ne 0) { throw 'could not build the script shop' }
        & $python.Source (Join-Path $repoRoot 'tooling\site\build-docs.py')
        if ($LASTEXITCODE -ne 0) { throw 'could not render the reference pages' }

        # The API page fetches the specification at runtime, so it has to sit
        # beside it - the same copy the Pages workflow makes.
        Copy-Item (Join-Path $repoRoot 'docs\openapi.yaml') `
                  (Join-Path $repoRoot 'site\api\openapi.yaml') -Force

        # The emulator is not published to Pages any more, but it is still
        # worth having here: its links up to / and /api/ only resolve in this
        # shape, so this is the one place they can be checked.
        $emulator = Join-Path $repoRoot 'simulator\web\public'
        $into = Join-Path $repoRoot 'site\emulator'
        if (Test-Path $emulator) {
            New-Item -ItemType Directory -Force $into | Out-Null
            Copy-Item (Join-Path $emulator '*') $into -Force -ErrorAction SilentlyContinue
            $built = Get-ChildItem $into -Filter 'stipple-core.*' -ErrorAction SilentlyContinue
            if (-not $built) {
                Write-Host "  note: the emulator has not been built, so /emulator/ will not run." -ForegroundColor DarkYellow
                Write-Host "        build it with '.\dev.ps1 emulator' (needs EMSDK)." -ForegroundColor DarkYellow
            }
        }

        Write-Host "`nhttp://localhost:8081/  (Ctrl+C to stop)`n" -ForegroundColor Cyan
        Push-Location (Join-Path $repoRoot 'site')
        try { & $python.Source -m http.server 8081 } finally { Pop-Location }
    }

    'panel' {
        # Cross-builds the channel-order test and runs it on the real matrix.
        #
        # This is the one question the spidev capture could not answer: every
        # lit pixel the vendor app drew was white, and white is the same bytes
        # under RGB, GRB and BGR. The answer comes from a person looking at the
        # panel, so the job here is to get the binary in front of them with as
        # few steps as possible.
        $target = if ($Rest) { $Rest[0] } else { $null }

        $adb = Get-Command adb -ErrorAction SilentlyContinue
        if (-not $adb) { throw "adb not found on PATH. See docs/bring-up.md for how to get it." }

        $engine = (Get-Command podman -ErrorAction SilentlyContinue) ??
                  (Get-Command docker -ErrorAction SilentlyContinue)
        if (-not $engine) {
            throw "podman or docker is needed to run the pinned cross-toolchain. See tooling/cross/."
        }

        # Static, so the bookworm image is fine — the __libc_start_main problem
        # only bites dynamically linked executables.
        $image = 'stipple-cross:bookworm'
        & $engine.Source build -t $image -f tooling/cross/Containerfile tooling/cross
        if ($LASTEXITCODE -ne 0) { throw "could not build the cross-toolchain image" }

        $script = @'
set -e
cmake --preset device-arm
cmake --build --preset device-arm --target stipple_panel_test
file /src/build/device-arm/firmware/stipple_panel_test
'@
        $script = $script -replace "`r", ""

        & $engine.Source run --rm -v "${repoRoot}:/src" $image bash -c $script
        if ($LASTEXITCODE -ne 0) { throw "cross-build failed" }

        $binary = Join-Path $repoRoot 'build\device-arm\firmware\stipple_panel_test'
        if (-not (Test-Path $binary)) { throw "expected $binary after the build" }

        if ($target) {
            & $adb.Source connect $target | Out-Null
        }

        # /tmp is the volatile path — a power cycle wipes it, which is exactly
        # what we want from something that takes the panel away from zkgui.
        & $adb.Source push $binary /tmp/stipple_panel_test
        if ($LASTEXITCODE -ne 0) { throw "adb push failed — is the device connected?" }

        & $adb.Source shell chmod 700 /tmp/stipple_panel_test

        Write-Host "`nLook at the panel." -ForegroundColor Green
        Write-Host "  Three bands, one channel each, left to right." -ForegroundColor Gray
        Write-Host "  Leftmost colour is channel 0, middle is 1, right is 2." -ForegroundColor Gray
        Write-Host "  One white dot top-left, two white pixels top-right." -ForegroundColor Gray
        Write-Host ""

        & $adb.Source shell /tmp/stipple_panel_test
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    'golden' {
        Invoke-Build 'host-debug'
        $binary = Get-TestBinary
        Write-Host "Regenerating golden fixtures. Review the PNGs in firmware\tests\testdata before committing." -ForegroundColor Yellow
        $env:STIPPLE_UPDATE_GOLDEN = '1'
        try { & $binary } finally { Remove-Item env:STIPPLE_UPDATE_GOLDEN -ErrorAction SilentlyContinue }
    }

    'preview' {
        # Renders frames to PNG through the simulator adapter. Not the emulator:
        # no input, nothing interactive — but it needs no Emscripten toolchain.
        Invoke-Build 'host-debug'

        $binary = Get-ChildItem (Join-Path $repoRoot "build\host-debug\firmware") -Recurse -Filter "stipple_preview.exe" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $binary) { throw "stipple_preview not found — did the build succeed?" }

        $outDir = Join-Path $repoRoot 'build\preview'
        New-Item -ItemType Directory -Force -Path $outDir | Out-Null

        & $binary.FullName $outDir
        if ($LASTEXITCODE -ne 0) { throw "preview render failed" }

        $page = Join-Path $outDir 'index.html'
        Write-Host "`nOpening $page" -ForegroundColor Green
        Start-Process $page
    }

    'emulator' {
        if (-not $env:EMSDK) {
            throw "EMSDK is not set. Install the Emscripten SDK, then run its emsdk_env.ps1 in this shell. See docs/development/toolchain.md"
        }
        Invoke-Build 'emulator'
        Write-Host "`nEmulator built into simulator\web\public." -ForegroundColor Green
    }

    'serve' {
        if ($env:EMSDK) { Invoke-Build 'emulator' }
        else { Write-Host "EMSDK not set — serving whatever is already in simulator\web\public." -ForegroundColor Yellow }

        $webRoot = Join-Path $repoRoot 'simulator\web\public'
        if (-not (Test-Path $webRoot)) { throw "no emulator build found at $webRoot" }

        # Resolve python.exe explicitly. A bare "python" on Windows often hits
        # the Microsoft Store app-execution alias, which is a stub that opens the
        # Store rather than running anything.
        $python = Get-Command python.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -notlike '*WindowsApps*' } |
            Select-Object -First 1
        if (-not $python) { throw "python.exe not found; needed for the static dev server" }

        Write-Host "Serving $webRoot at http://localhost:8080/ (Ctrl+C to stop)" -ForegroundColor Green
        & $python.Source -m http.server 8080 --directory $webRoot
    }

    'clean' {
        $buildDir = Join-Path $repoRoot 'build'
        if (Test-Path $buildDir) {
            Remove-Item $buildDir -Recurse -Force
            Write-Host "Removed $buildDir" -ForegroundColor Green
        } else {
            Write-Host "Nothing to clean."
        }
    }

    'doctor' {
        Write-Host "STIPPLE toolchain status`n" -ForegroundColor Cyan

        try { $cmake = Find-CMake; Write-Host ("  cmake       OK    " + $cmake) -ForegroundColor Green }
        catch { Write-Host "  cmake       MISSING" -ForegroundColor Red }

        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) {
            $vsPath = & $vswhere -latest -products * -property installationPath
            $msvc = if ($vsPath) { Join-Path $vsPath 'VC\Tools\MSVC' } else { $null }
            if ($msvc -and (Test-Path $msvc)) {
                $toolset = (Get-ChildItem $msvc -Name | Select-Object -Last 1)
                Write-Host "  MSVC        OK    $toolset" -ForegroundColor Green
            } else {
                Write-Host "  MSVC        MISSING  (add 'Desktop development with C++' in the VS Installer)" -ForegroundColor Red
            }
        } else {
            Write-Host "  MSVC        UNKNOWN  (vswhere not found)" -ForegroundColor Yellow
        }

        if ($env:EMSDK) { Write-Host ("  emscripten  OK    " + $env:EMSDK) -ForegroundColor Green }
        else { Write-Host "  emscripten  MISSING  (needed only for 'dev.ps1 emulator')" -ForegroundColor Yellow }

        $python = Get-Command python.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.Source -notlike '*WindowsApps*' } | Select-Object -First 1
        if ($python) { Write-Host ("  python      OK    " + $python.Source) -ForegroundColor Green }
        else { Write-Host "  python      MISSING  (needed only for 'dev.ps1 serve')" -ForegroundColor Yellow }

        Write-Host "`n  TC002       not required until Phase 7" -ForegroundColor DarkGray
    }
}
