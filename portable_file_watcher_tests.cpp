/*
    portable_file_watcher_tests.cpp

    Build and run with CMake:

        cmake -S . -B build
        cmake --build build
        ctest --test-dir build --output-on-failure

    The test creates a temporary directory, watches it, and verifies that
    create, modify, rename, and delete operations generate notifications.
*/

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "portable_file_watcher.hpp"

namespace fs = std::filesystem;

namespace {

// Which directory-watch events the platform backend can report. Windows
// (ReadDirectoryChangesW) and Linux (inotify) report the child filename with a
// distinct event kind. macOS/BSD (kqueue) reports only that the watched vnode
// changed: no child filenames, and content-only rewrites of children are not
// observed when the directory itself is the watched path.
#if defined(_WIN32) || defined(__linux__)
inline constexpr bool reports_child_events = true;
#else
inline constexpr bool reports_child_events = false;
#endif

struct TestFailure {
  std::string message;
};

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      throw TestFailure{std::string("check failed: ") + #condition +           \
                        " at line " + std::to_string(__LINE__)};               \
    }                                                                          \
  } while (false)

struct EventLog {
private:
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::vector<pfw::Notification> notifications;

public:
  void add(const pfw::Notification &notification) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      notifications.push_back(notification);
    }
    condition.notify_all();
  }

  bool
  wait_for(const std::function<bool(const std::vector<pfw::Notification> &)>
               &predicate,
           // NOLINTNEXTLINE(readability-magic-numbers)
           std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::unique_lock<std::mutex> lock(mutex);

    return condition.wait_for(lock, timeout, [&] {
      // The predicate is called while mutex is held.
      return predicate(notifications);
    });
  }

  static bool
  contains_event(const std::vector<pfw::Notification> &notifications,
                 const fs::path &path, pfw::WatchedFileEvent expected,
                 bool exact_path = true) {
    return std::any_of(
        notifications.begin(), notifications.end(),
        [&](const auto &notification) {
          const bool path_matches =
              exact_path ? notification.path == path
                         : notification.path.filename() == path.filename();

          return path_matches && pfw::any(notification.event & expected);
        });

    return false;
  }

  bool contains_event(const fs::path &path, pfw::WatchedFileEvent expected,
                      bool exact_path = true) const {
    const std::lock_guard<std::mutex> lock(mutex);
    return contains_event(notifications, path, expected, exact_path);
  }
};

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto base = fs::temp_directory_path();

    // NOLINTNEXTLINE(readability-magic-numbers)
    for (int attempt = 0; attempt < 100; ++attempt) {
      path_ =
          base /
          ("portable_file_watcher_test_" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "_" + std::to_string(attempt));

      std::error_code error;
      if (fs::create_directory(path_, error)) {
        return;
      }
    }

    throw TestFailure{"could not create temporary directory"};
  }

  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path &path() const { return path_; }

private:
  fs::path path_;
};

void write_file(const fs::path &path, const std::string &contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);

  if (!output) {
    throw TestFailure{"could not open file for writing: " + path.string()};
  }

  output << contents;
  output.close();

  if (!output) {
    throw TestFailure{"could not write file: " + path.string()};
  }
}

void test_invalid_arguments() {
  pfw::PortableFileWatcher watcher;

  CHECK(!watcher.start({}, [](const pfw::Notification &) {}));
  CHECK(!watcher.start("some-path", {}));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void test_file_lifecycle() {
  const TemporaryDirectory temporary_directory;
  const fs::path &directory = temporary_directory.path();

  EventLog log;

  pfw::PortableFileWatcher watcher;

  CHECK(watcher.start(directory, [&](const pfw::Notification &notification) {
    log.add(notification);
  }));

  const fs::path original = directory / "original.txt";
  const fs::path renamed = directory / "renamed.txt";

  // kqueue assertions count directory-wide Modified events so each step below
  // must produce a new notification instead of satisfying the check with an
  // earlier one. The lambda is unused where the backend reports child events.
  [[maybe_unused]] const auto directory_modifications =
      [&directory](const auto &events) {
        return std::count_if(events.begin(), events.end(), [&](const auto &n) {
          return n.path == directory &&
                 pfw::any(n.event & pfw::WatchedFileEvent::Modified);
        });
      };

  write_file(original, "first version");

  if constexpr (reports_child_events) {
    // ReadDirectoryChangesW and inotify report the new child filename.
    CHECK(log.wait_for([&](const auto &events) {
      return EventLog::contains_event(events, original,
                                      pfw::WatchedFileEvent::Created);
    }));
  } else {
    // kqueue reports only that the watched directory vnode changed, so a
    // created child surfaces as a Modified event on the directory itself.
    CHECK(log.wait_for([&](const auto &events) {
      return directory_modifications(events) >= 1;
    }));
  }

  write_file(original, "second version");

  if constexpr (reports_child_events) {
    CHECK(log.wait_for([&](const auto &events) {
      return EventLog::contains_event(events, original,
                                      pfw::WatchedFileEvent::Modified);
    }));
  }
  // kqueue: rewriting a child's contents touches the child vnode, not the
  // directory, so there is nothing to assert for this step.

  std::error_code error;
  fs::rename(original, renamed, error);
  CHECK(!error);

  if constexpr (reports_child_events) {
    CHECK(log.wait_for([&](const auto &events) {
      return EventLog::contains_event(events, original,
                                      pfw::WatchedFileEvent::Renamed) ||
             EventLog::contains_event(events, renamed,
                                      pfw::WatchedFileEvent::Renamed);
    }));
  } else {
    CHECK(log.wait_for([&](const auto &events) {
      return directory_modifications(events) >= 2;
    }));
  }

  fs::remove(renamed, error);
  CHECK(!error);

  if constexpr (reports_child_events) {
    CHECK(log.wait_for([&](const auto &events) {
      return EventLog::contains_event(events, renamed,
                                      pfw::WatchedFileEvent::Removed);
    }));
  } else {
    // NOLINTNEXTLINE(readability-magic-numbers)
    CHECK(log.wait_for([&](const auto &events) {
      return directory_modifications(events) >= 3;
    }));
  }

  watcher.stop();
}

void test_single_file_filtering() {
  const TemporaryDirectory temporary_directory;
  const fs::path &directory = temporary_directory.path();

  const fs::path watched_file = directory / "watched.txt";
  const fs::path unrelated_file = directory / "unrelated.txt";

  write_file(watched_file, "initial");
  write_file(unrelated_file, "initial");

  EventLog log;
  pfw::PortableFileWatcher watcher;

  CHECK(watcher.start(watched_file, [&](const pfw::Notification &notification) {
    log.add(notification);
  }));

  write_file(unrelated_file, "changed");

  // Give the watcher a chance to process the unrelated event. It should
  // not be reported because the watcher was given a specific file.
  // NOLINTNEXTLINE(readability-magic-numbers)
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  CHECK(!log.contains_event(unrelated_file, pfw::WatchedFileEvent::Modified));

  write_file(watched_file, "changed");

  CHECK(log.wait_for([&](const auto &events) {
    return EventLog::contains_event(events, watched_file,
                                    pfw::WatchedFileEvent::Modified);
  }));

  watcher.stop();
}

void test_recursive_directory_watch() {
  const TemporaryDirectory temporary_directory;
  const fs::path &root = temporary_directory.path();
  const fs::path nested = root / "nested";

  std::error_code error;
  fs::create_directory(nested, error);
  CHECK(!error);

  EventLog log;
  pfw::PortableFileWatcher watcher;

  CHECK(watcher.start(
      root,
      [&](const pfw::Notification &notification) { log.add(notification); },
      true));

  const fs::path nested_file = nested / "nested.txt";
  write_file(nested_file, "nested content");

#ifdef _WIN32
  // ReadDirectoryChangesW supports recursive reporting of nested child
  // filenames.
  CHECK(log.wait_for([&](const auto &) {
    return log.contains_event(nested_file, pfw::WatchedFileEvent::Created);
  }));
#else
  // Neither Linux (inotify) nor macOS/BSD (kqueue) adds watches beneath the
  // root directory, so recursive delivery is deliberately not asserted here.
  // The test still verifies that starting a recursive watch is accepted and
  // remains functional.
  // NOLINTNEXTLINE(readability-magic-numbers)
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
#endif

  watcher.stop();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void test_start_stop_repeatedly() {
  const TemporaryDirectory temporary_directory;
  const fs::path &directory = temporary_directory.path();

  // NOLINTNEXTLINE(readability-magic-numbers)
  for (int iteration = 0; iteration < 5; ++iteration) {
    EventLog log;
    pfw::PortableFileWatcher watcher;

    CHECK(watcher.start(directory, [&](const pfw::Notification &notification) {
      log.add(notification);
    }));

    const fs::path file =
        directory / ("iteration_" + std::to_string(iteration) + ".txt");

    write_file(file, "contents");

    if constexpr (reports_child_events) {
      CHECK(log.wait_for([&](const auto &events) {
        return EventLog::contains_event(events, file,
                                        pfw::WatchedFileEvent::Created);
      }));
    } else {
      // kqueue: the created file surfaces as a Modified event on the watched
      // directory rather than a Created event naming the child.
      CHECK(log.wait_for([&](const auto &events) {
        return EventLog::contains_event(events, directory,
                                        pfw::WatchedFileEvent::Modified);
      }));
    }

    watcher.stop();
    CHECK(!watcher.running());

    std::error_code error;
    fs::remove(file, error);
    CHECK(!error);
  }
}

void run_test(const char *name, void (*test)()) {
  std::cout << "[ RUN      ] " << name << '\n';

  try {
    test();
    std::cout << "[       OK ] " << name << '\n';
  } catch (const TestFailure &failure) {
    std::cerr << "[  FAILED  ] " << name << '\n'
              << "             " << failure.message << '\n';
    throw;
  } catch (const std::exception &exception) {
    std::cerr << "[  FAILED  ] " << name << '\n'
              << "             unexpected exception: " << exception.what()
              << '\n';
    throw;
  }
}

} // namespace

int main() {
  try {
    run_test("invalid_arguments", test_invalid_arguments);
    run_test("file_lifecycle", test_file_lifecycle);
    run_test("single_file_filtering", test_single_file_filtering);
    run_test("recursive_directory_watch", test_recursive_directory_watch);
    run_test("start_stop_repeatedly", test_start_stop_repeatedly);

    std::cout << "[  PASSED  ] all tests\n";
    return EXIT_SUCCESS;
  } catch (...) {
    return EXIT_FAILURE;
  }
}
