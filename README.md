# Portable File Watcher

A single-header, cross-platform C++17 file-system watcher for Windows, Linux, macOS, and BSD systems.

The library uses native operating-system notification APIs:

- **Windows:** `ReadDirectoryChangesW`
- **Linux:** `inotify`
- **macOS and BSD:** `kqueue`

It provides a small callback-based API for monitoring files and directories without external dependencies.

> **Status:** Header-only and experimental. Review the platform limitations below before using it in production.

## Features

- Single-header implementation
- No third-party dependencies
- C++17 `std::filesystem` paths
- File and directory watching
- Optional recursive watching on Windows
- Event flags for:
  - Creation
  - Modification
  - Removal
  - Rename
  - Queue overflow
  - Errors
- RAII-compatible lifetime management
- Callback exceptions are contained and do not escape the watcher thread

## Requirements

- C++17 or newer
- A supported platform:
  - Windows
  - Linux
  - macOS
  - FreeBSD
  - OpenBSD
  - NetBSD

The implementation requires access to the platform's native file notification APIs.

## Installation

Copy `portable_file_watcher.hpp` into your project and include it.

The header may be included in any number of source files, but the
implementation is emitted in exactly one translation unit. In **one** of your
`.cpp` files, define `PFW_IMPLEMENTATION` before including:

```cpp
#define PFW_IMPLEMENTATION
#include "portable_file_watcher.hpp"
```

Every other source file includes the header without the macro:

```cpp
#include "portable_file_watcher.hpp"
```

Those translation units see declarations only and link against the file that
defines `PFW_IMPLEMENTATION`. Defining it in more than one translation unit is
a link error. No separate library build or linking step is required.

## Basic usage

```cpp
#define PFW_IMPLEMENTATION
#include "portable_file_watcher.hpp"

#include <iostream>

int main() {
    pfw::PortableFileWatcher watcher;

    if (!watcher.start(
            "config.json",
            [](const pfw::Notification& notification) {
                std::cout
                    << "Changed: " << notification.path
                    << '\n';
            })) {
        std::cerr << "Could not start watcher\n";
        return 1;
    }

    std::cout << "Watching; press Enter to stop...\n";
    std::cin.get();

    watcher.stop();
}
```

The watcher can also be constructed and started in one operation:

```cpp
#include "portable_file_watcher.hpp"

#include <iostream>

int main() {
    pfw::PortableFileWatcher watcher(
        "config.json",
        [](const pfw::Notification& notification) {
            std::cout << "Changed: "
                      << notification.path << '\n';
        });

    if (!watcher.running()) {
        std::cerr << "Could not start watcher\n";
        return 1;
    }

    std::cin.get();
}
```

## Watching a directory

Pass a directory path to receive notifications for entries within that directory:

```cpp
pfw::PortableFileWatcher watcher(
    "assets",
    [](const pfw::Notification& notification) {
        std::cout << "Asset event: "
                  << notification.path << '\n';
    });
```

The callback receives the path of the changed entry along with the event type.

## Recursive watching

The `recursive` argument can be enabled when starting a watcher:

```cpp
pfw::PortableFileWatcher watcher;

if (!watcher.start(
        "assets",
        [](const pfw::Notification& notification) {
            std::cout << notification.path << '\n';
        },
        true)) {
    return 1;
}
```

Recursive behavior is platform-dependent. See [Platform limitations](#platform-limitations).

## API

### `PortableFileWatcher`

```cpp
class PortableFileWatcher;
```

#### Default constructor

```cpp
pfw::PortableFileWatcher watcher;
```

Creates an inactive watcher.

#### Path-and-callback constructor

```cpp
pfw::PortableFileWatcher(
    const std::filesystem::path& path,
    Callback callback,
    bool recursive = false);
```

Creates and starts a watcher.

#### `start`

```cpp
bool start(
    const std::filesystem::path& path,
    Callback callback,
    bool recursive = false);
```

Stops any existing watch and starts watching `path`.

Returns `true` when the underlying platform watcher starts successfully. Returns `false` if:

- The path is empty
- The callback is empty
- The target directory or file cannot be opened
- The native watcher cannot be initialized

#### `stop`

```cpp
void stop();
```

Stops the watcher and waits for its worker thread to finish.

Calling `stop()` multiple times is safe.

#### `running`

```cpp
bool running() const;
```

Returns whether the watcher has an active worker thread and has not been stopped.

## Notifications

Callbacks receive a `pfw::Notification`:

```cpp
struct Notification {
    std::filesystem::path path;
    WatchedFileEvent event = WatchedFileEvent::None;
};
```

The `path` member identifies the affected file or directory.

The `event` member is a bitmask of `WatchedFileEvent` values.

## Event types

```cpp
enum class WatchedFileEvent : std::uint32_t {
    None      = 0,
    Created   = 1u << 0,
    Modified  = 1u << 1,
    Removed   = 1u << 2,
    Renamed   = 1u << 3,
    Overflow  = 1u << 4,
    Error     = 1u << 5
};
```

Events can be combined:

```cpp
if (pfw::any(notification.event &
            pfw::WatchedFileEvent::Modified)) {
    std::cout << "The file was modified\n";
}
```

Multiple flags may be set for one notification. For example, a rename notification may also include removal or modification information depending on the platform.

## Recommended event handling

Operating systems and applications commonly represent one logical save operation as multiple file-system operations. An editor may:

1. Write a temporary file
2. Rename the original file
3. Rename the temporary file into place
4. Update file metadata

For this reason, applications should generally handle `Created`, `Modified`, `Removed`, and `Renamed` as related events rather than assuming each event represents an independent user action.

A debounce mechanism is recommended for applications that reload files or perform expensive work:

```cpp
// Pseudocode: enqueue notifications and process them after
// a short quiet period rather than immediately in the callback.
```

## Threading

Callbacks execute on the watcher’s worker thread.

Callbacks should be short and non-blocking. If processing is expensive, enqueue the notification for handling on another thread:

```cpp
pfw::PortableFileWatcher watcher(
    "config.json",
    [](const pfw::Notification& notification) {
        // Push notification into a thread-safe queue.
        // Do not perform expensive work here.
    });
```

The callback is copied while protected by an internal mutex. Exceptions thrown by callbacks are caught and ignored so they cannot terminate the watcher thread.

## Platform limitations

### Windows

- Watches the containing directory when a single file is requested.
- File events are filtered when watching a specific file.
- Recursive directory watching is supported through `ReadDirectoryChangesW`.
- A notification may be generated for a temporary file used by an editor during an atomic save.
- `start()` does not return until the watcher's first change-read is queued, so
  changes made immediately after `start()` returns are not missed.

### Linux

- Uses `inotify`.
- The current implementation adds an `inotify` watch for only one directory.
- Recursive watching is not fully implemented. The `recursive` parameter does not create watches for every nested subdirectory.
- `IN_Q_OVERFLOW` produces an `Overflow` notification. Applications should rescan the watched directory when this occurs.
- Events may be reported multiple times for one logical file operation.

### macOS and BSD

- Uses `kqueue`.
- The watcher monitors vnode changes on the target path.
- `kqueue` reports changes to the watched vnode, not exact child filenames in a directory.
- Exact recursive directory monitoring is not implemented.
- For robust recursive directory monitoring on macOS, an FSEvents-based backend would generally be more appropriate.
- A renamed or removed target may cause the worker thread to stop.

### General limitations

- Notifications are best-effort and platform-dependent.
- Event ordering and event granularity vary by operating system.
- A callback may be invoked more than once for a single logical operation.
- Applications should rescan state after overflow or other error notifications.
- The watcher does not provide built-in debouncing.
- The callback must not assume that the reported path still exists.

## CMake example

A header-only CMake target can be declared as follows:

```cmake
cmake_minimum_required(VERSION 3.16)

project(file_watcher_example LANGUAGES CXX)

add_executable(file_watcher_example
    main.cpp
)

target_compile_features(file_watcher_example PRIVATE cxx_std_17)

target_include_directories(file_watcher_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
```

Place `portable_file_watcher.hpp` in the project's `include` directory.

## Error handling

`start()` reports initialization failures through its return value:

```cpp
pfw::PortableFileWatcher watcher;

if (!watcher.start(
        "data",
        [](const pfw::Notification& notification) {
            if (pfw::any(notification.event &
                         pfw::WatchedFileEvent::Error)) {
                std::cerr << "Watcher error\n";
            }
        })) {
    std::cerr << "Failed to initialize file watcher\n";
}
```

An `Overflow` notification indicates that the operating-system event queue lost events. The application should perform a full rescan instead of relying only on individual notifications.

## License

Copyright 2026 Ebben Feagan.

This project is licensed under the MIT License. See the license header in `portable_file_watcher.hpp` for the full license text.

