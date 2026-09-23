/*
 * Copyright 2026 Ebben Feagan
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

/*
 * Usage
 * -----
 *
 * This is a single-header library: it may be included in any number of
 * translation units, but the implementation is emitted in exactly one.
 *
 * In ONE of your source files, define PFW_IMPLEMENTATION before including:
 *
 *     #define PFW_IMPLEMENTATION
 *     #include "portable_file_watcher.hpp"
 *
 * Every other source file includes the header normally (without the macro);
 * those translation units see declarations only and link against the file
 * above. Defining PFW_IMPLEMENTATION in more than one translation unit is a
 * link error.
 */

// NOLINTNEXTLINE(portability-avoid-pragma-once)
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <future>
#include <windows.h>

#elif defined(__linux__)

#include <cerrno>
#include <sys/inotify.h>
#include <unistd.h>

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
    defined(__NetBSD__)

#include <cerrno>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#else
#error "portable_file_watcher: unsupported platform"
#endif

/**
 * A few important limitations:
 *
 *    The implementation watches a directory on Windows and Linux, then filters
 * events when a single file was requested. Linux recursive watching is not
 * fully implemented here. inotify requires a separate watch descriptor for
 * every subdirectory. macOS and BSD kqueue reports vnode changes but not the
 * individual child filename. For exact recursive directory events on macOS, an
 * FSEvents backend would be preferable. Editors often save through a temporary
 * file and rename it. Applications should treat Renamed, Removed, and Created
 * as possible parts of one save operation. Callbacks execute on the watcher
 * thread. If the callback performs expensive work, enqueue the notification
 * elsewhere. A callback may be generated more than once for a single logical
 * file save, so debouncing is often useful.
 */
namespace pfw {

inline constexpr int version_major = 1;
inline constexpr int version_minor = 0;
inline constexpr int version_patch = 0;

inline constexpr char version_string[] = "1.0.0";
inline constexpr int version =
    (version_major * 10000) + (version_minor * 100) + version_patch;

enum class WatchedFileEvent : std::uint8_t {
  None = 0,
  Created = 1U << 0,
  Modified = 1U << 1,
  Removed = 1U << 2,
  Renamed = 1U << 3,
  Overflow = 1U << 4,
  Error = 1U << 5
};

WatchedFileEvent operator|(WatchedFileEvent lhs, WatchedFileEvent rhs);
WatchedFileEvent operator&(WatchedFileEvent lhs, WatchedFileEvent rhs);
bool any(WatchedFileEvent event);

struct Notification {
  std::filesystem::path path;
  WatchedFileEvent event = WatchedFileEvent::None;
};

class PortableFileWatcher {
public:
  using Callback = std::function<void(const Notification &)>;

  PortableFileWatcher() = default;

  explicit PortableFileWatcher(const std::filesystem::path &path,
                               const Callback &callback,
                               bool recursive = false);

  ~PortableFileWatcher();

  PortableFileWatcher(const PortableFileWatcher &) = delete;
  PortableFileWatcher &operator=(const PortableFileWatcher &) = delete;

  bool start(const std::filesystem::path &path, Callback callback,
             bool recursive = false);

  void stop();

  bool running() const;

private:
  std::filesystem::path path_;
  Callback callback_;
  bool recursive_ = false;
  std::atomic<bool> stopping_{false};
  std::thread worker_;

#ifdef _WIN32

  HANDLE directory_handle_ = INVALID_HANDLE_VALUE;

  static std::wstring wide(const std::filesystem::path &path);

  bool start_windows();

#elif defined(__linux__)

  int inotify_fd_ = -1;
  int watch_descriptor_ = -1;

  bool start_linux();

#else

  int kqueue_fd_ = -1;
  int watched_fd_ = -1;

  bool start_kqueue();

#endif

  void emit(const std::filesystem::path &changed, WatchedFileEvent event);

  std::mutex callback_mutex_;
};

/**
 * @example File Watcher
 * ```cpp
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
 */

/**
 * @example Directory watcher
```
pfw::PortableFileWatcher watcher(
    "assets",
    [](const pfw::Notification& n) {
    std::cout << "Asset event: " << n.path << '\n';
},
true
);

```
*/
} // namespace pfw

// -----------------------------------------------------------------------------
// Implementation section — the bodies above are emitted only in the single
// translation unit that defines PFW_IMPLEMENTATION before including this
// header. See the Usage comment at the top of the file.
// -----------------------------------------------------------------------------
#ifdef PFW_IMPLEMENTATION

// NOLINTBEGIN(misc-definitions-in-headers)
namespace pfw {

WatchedFileEvent operator|(WatchedFileEvent lhs, WatchedFileEvent rhs) {
  return static_cast<WatchedFileEvent>(static_cast<std::uint32_t>(lhs) |
                                       static_cast<std::uint32_t>(rhs));
}

WatchedFileEvent operator&(WatchedFileEvent lhs, WatchedFileEvent rhs) {
  return static_cast<WatchedFileEvent>(static_cast<std::uint32_t>(lhs) &
                                       static_cast<std::uint32_t>(rhs));
}

bool any(WatchedFileEvent event) {
  return static_cast<std::uint32_t>(event) != 0;
}

PortableFileWatcher::PortableFileWatcher(const std::filesystem::path &path,
                                         const Callback &callback,
                                         bool recursive) {
  start(path, callback, recursive);
}

PortableFileWatcher::~PortableFileWatcher() { stop(); }

bool PortableFileWatcher::start(const std::filesystem::path &path,
                                Callback callback, bool recursive) {
  stop();

  if (path.empty() || !callback) {
    return false;
  }

  path_ = std::filesystem::absolute(path);
  callback_ = std::move(callback);
  recursive_ = recursive;
  stopping_ = false;

#ifdef _WIN32
  return start_windows();
#elif defined(__linux__)
  return start_linux();
#else
  return start_kqueue();
#endif
}

void PortableFileWatcher::stop() {
  stopping_ = true;

#ifdef _WIN32
  if (directory_handle_ != INVALID_HANDLE_VALUE) {
    CancelIoEx(directory_handle_, nullptr);
  }
#elif defined(__linux__)
  if (inotify_fd_ >= 0) {
    ::close(inotify_fd_);
    inotify_fd_ = -1;
  }
#else
  if (kqueue_fd_ >= 0) {
    ::close(kqueue_fd_);
    kqueue_fd_ = -1;
  }

  if (watched_fd_ >= 0) {
    ::close(watched_fd_);
    watched_fd_ = -1;
  }
#endif

  if (worker_.joinable()) {
    worker_.join();
  }

#ifdef _WIN32
  if (directory_handle_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(directory_handle_);
    directory_handle_ = INVALID_HANDLE_VALUE;
  }
#endif
}

bool PortableFileWatcher::running() const {
  return worker_.joinable() && !stopping_;
}

#ifdef _WIN32

std::wstring PortableFileWatcher::wide(const std::filesystem::path &path) {
  return path.wstring();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool PortableFileWatcher::start_windows() {
  const std::filesystem::path directory =
      std::filesystem::is_directory(path_) ? path_ : path_.parent_path();

  directory_handle_ =
      ::CreateFileW(wide(directory).c_str(), FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);

  if (directory_handle_ == INVALID_HANDLE_VALUE) {
    return false;
  }

  path_ = std::filesystem::absolute(path_);

  // ReadDirectoryChangesW only reports changes that occur after its first
  // call, so start() must not return until the worker has one outstanding:
  // otherwise a caller that mutates the tree immediately after start() loses
  // those events to the thread-startup race. The first read therefore uses
  // overlapped I/O, which is queued (not blocked on a change) so the worker
  // can signal readiness, and start() waits for that signal. Overlapped reads
  // require the handle to be opened with FILE_FLAG_OVERLAPPED (see
  // CreateFileW above): without it ReadDirectoryChangesW ignores the
  // OVERLAPPED structure and blocks until a change occurs, deadlocking
  // start().
  std::promise<bool> ready;
  std::future<bool> ready_future = ready.get_future();

  // Allocate the event buffer here and move it onto the worker: a bad_alloc
  // on this thread surfaces from start() instead of escaping a std::thread
  // body, which would call std::terminate.
  constexpr DWORD buffer_size = 64 * 1024;
  std::vector<std::uint8_t> buffer(buffer_size);

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  worker_ = std::thread([this, directory, ready = std::move(ready),
                         buffer = std::move(buffer)]() mutable {
    constexpr DWORD notify_filter =
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_CREATION;

    HANDLE completion_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (completion_event == nullptr) {
      ready.set_value(false);
      return;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = completion_event;

    const auto issue_read = [&]() {
      return ::ReadDirectoryChangesW(directory_handle_, buffer.data(),
                                     static_cast<DWORD>(buffer.size()),
                                     recursive_ ? TRUE : FALSE, notify_filter,
                                     nullptr, &overlapped, nullptr);
    };

    const BOOL first_ok = issue_read();
    ready.set_value(first_ok != FALSE);

    // Everything below allocates (event names, paths, the callback copy in
    // emit), and an exception must never escape a std::thread body: that
    // calls std::terminate. Report it the same way as the other watch-error
    // paths. No read is in flight while events are processed: the pending
    // read has already completed and the next one is queued only at the loop
    // bottom, so exiting with an Error event rejoins the no-read-in-flight
    // exit path.
    try {
      if (!first_ok) {
        if (!stopping_) {
          emit(path_, WatchedFileEvent::Error);
        }
      } else {
        for (;;) {
          DWORD transferred = 0;

          if (!::GetOverlappedResult(directory_handle_, &overlapped,
                                     &transferred, TRUE)) {
            // stop() cancels the pending read, completing it with
            // ERROR_OPERATION_ABORTED; any other failure is a watch error.
            // Either way the operation has finished, so nothing is in flight
            // when the worker exits.
            if (!stopping_ && ::GetLastError() != ERROR_OPERATION_ABORTED) {
              emit(path_, WatchedFileEvent::Error);
            }
            break;
          }

          // Manual-reset events stay signaled, so re-arm it for the next
          // read. Changes that occur between reads are buffered by the
          // kernel and returned with the next call, so nothing is lost.
          if (!::ResetEvent(completion_event)) {
            if (!stopping_) {
              emit(path_, WatchedFileEvent::Error);
            }
            break;
          }

          if (transferred != 0) {
            auto *record =
                reinterpret_cast<FILE_NOTIFY_INFORMATION *>(buffer.data());

            for (;;) {
              const std::wstring name(record->FileName,
                                      record->FileNameLength / sizeof(wchar_t));

              auto changed = directory / name;
              changed = std::filesystem::absolute(changed);

              WatchedFileEvent event = WatchedFileEvent::None;

              switch (record->Action) {
              case FILE_ACTION_ADDED:
                event = WatchedFileEvent::Created;
                break;
              case FILE_ACTION_REMOVED:
                event = WatchedFileEvent::Removed;
                break;
              case FILE_ACTION_MODIFIED:
                event = WatchedFileEvent::Modified;
                break;
              case FILE_ACTION_RENAMED_OLD_NAME:
              case FILE_ACTION_RENAMED_NEW_NAME:
                event = WatchedFileEvent::Renamed;
                break;
              default:
                // Unrecognized action; leave the event as None so the
                // any(event) check below filters it out.
                break;
              }

              if (any(event) &&
                  (std::filesystem::is_directory(path_) || changed == path_)) {
                emit(changed, event);
              }

              if (record->NextEntryOffset == 0) {
                break;
              }

              record = reinterpret_cast<FILE_NOTIFY_INFORMATION *>(
                  reinterpret_cast<std::uint8_t *>(record) +
                  record->NextEntryOffset);
            }
          }

          // Queue the next read only while still running, and never break
          // with a read in flight: its completion would write into this
          // thread's stack after it has exited. stop() may have cancelled
          // during event processing, in which case the loop re-checks here.
          if (stopping_) {
            break;
          }

          if (!issue_read()) {
            if (!stopping_) {
              emit(path_, WatchedFileEvent::Error);
            }
            break;
          }
        }
      }
    } catch (...) {
      if (!stopping_) {
        emit(path_, WatchedFileEvent::Error);
      }
    }

    // Every exit path above has no read in flight, so the completion
    // event can be released and the thread can exit safely.
    ::CloseHandle(completion_event);
  });

  if (!ready_future.get()) {
    stop();
    return false;
  }

  return true;
}

#elif defined(__linux__)

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool PortableFileWatcher::start_linux() {
  const std::filesystem::path directory =
      std::filesystem::is_directory(path_) ? path_ : path_.parent_path();

  inotify_fd_ = ::inotify_init1(IN_NONBLOCK);
  if (inotify_fd_ < 0) {
    return false;
  }

  const std::uint32_t mask =
      IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM |
      IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW;

  watch_descriptor_ = ::inotify_add_watch(inotify_fd_, directory.c_str(), mask);

  if (watch_descriptor_ < 0) {
    ::close(inotify_fd_);
    inotify_fd_ = -1;
    return false;
  }

  path_ = std::filesystem::absolute(path_);

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  worker_ = std::thread([this, directory] {
    // NOLINTNEXTLINE(readability-magic-numbers)
    std::vector<char> buffer(static_cast<size_t>(64 * 1024));

    while (!stopping_) {
      const ssize_t nbytes = ::read(inotify_fd_, buffer.data(), buffer.size());

      if (nbytes < 0) {
        if (errno == EAGAIN || errno == EINTR) {
          // NOLINTNEXTLINE(readability-magic-numbers)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }

        if (!stopping_) {
          emit(path_, WatchedFileEvent::Error);
        }
        break;
      }

      unsigned int offset = 0;

      while (offset < nbytes) {
        const auto *event =
            reinterpret_cast<const inotify_event *>(buffer.data() + offset);

        if ((event->mask & IN_Q_OVERFLOW) != 0) {
          emit(path_, WatchedFileEvent::Overflow);
        } else {
          std::filesystem::path changed = directory;

          if (event->len != 0) {
            changed /= event->name;
          }

          changed = std::filesystem::absolute(changed);

          WatchedFileEvent result = WatchedFileEvent::None;

          if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
            result = result | WatchedFileEvent::Created;
          }

          if ((event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB)) != 0) {
            result = result | WatchedFileEvent::Modified;
          }

          if ((event->mask & (IN_DELETE | IN_DELETE_SELF)) != 0) {
            result = result | WatchedFileEvent::Removed;
          }

          if ((event->mask & (IN_MOVED_FROM | IN_MOVE_SELF)) != 0) {
            result = result | WatchedFileEvent::Renamed;
          }

          if (any(result) &&
              (std::filesystem::is_directory(path_) || changed == path_)) {
            emit(changed, result);
          }
        }

        offset += sizeof(inotify_event) + event->len;
      }
    }
  });

  return true;
}

#else

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
bool PortableFileWatcher::start_kqueue() {
#ifdef __APPLE__
  watched_fd_ = ::open(path_.c_str(), O_EVTONLY);
#else
  watched_fd_ = ::open(path_.c_str(), O_RDONLY);
#endif

  if (watched_fd_ < 0) {
    return false;
  }

  kqueue_fd_ = ::kqueue();

  if (kqueue_fd_ < 0) {
    ::close(watched_fd_);
    watched_fd_ = -1;
    return false;
  }

  struct kevent change;
  EV_SET(&change, watched_fd_, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR,
         NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_EXTEND |
             NOTE_LINK,
         0, nullptr);

  if (::kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr) < 0) {
    ::close(kqueue_fd_);
    ::close(watched_fd_);
    kqueue_fd_ = -1;
    watched_fd_ = -1;
    return false;
  }

  worker_ = std::thread([this] {
    while (!stopping_) {
      struct kevent event;
      const timespec timeout{1, 0};

      const int num_events =
          ::kevent(kqueue_fd_, nullptr, 0, &event, 1, &timeout);

      if (num_events < 0) {
        if (errno == EINTR) {
          continue;
        }

        if (!stopping_) {
          emit(path_, WatchedFileEvent::Error);
        }
        break;
      }

      if (num_events == 0) {
        continue;
      }

      WatchedFileEvent result = WatchedFileEvent::Modified;

      if ((event.fflags & NOTE_DELETE) != 0) {
        result = result | WatchedFileEvent::Removed;
      }

      if ((event.fflags & NOTE_RENAME) != 0) {
        result = result | WatchedFileEvent::Renamed;
      }

      if ((event.fflags & NOTE_ATTRIB) != 0) {
        result = result | WatchedFileEvent::Modified;
      }

      emit(path_, result);

      if ((event.fflags & (NOTE_DELETE | NOTE_RENAME)) != 0) {
        break;
      }
    }
  });

  return true;
}

#endif

void PortableFileWatcher::emit(const std::filesystem::path &changed,
                               WatchedFileEvent event) {
  Callback callback;

  {
    const std::lock_guard<std::mutex> lock(callback_mutex_);
    callback = callback_;
  }

  if (callback && !stopping_) {
    try {
      callback(Notification{changed, event});
    } catch (...) { // NOLINT(bugprone-empty-catch)
      // Exceptions must not escape the watcher thread.
    }
  }
}

} // namespace pfw
// NOLINTEND(misc-definitions-in-headers)
#endif // PFW_IMPLEMENTATION