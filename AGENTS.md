# AGENTS.md

Single-header C++17 file watcher (`namespace pfw`) with no dependencies and no
library build. All library code lives in `portable_file_watcher.hpp`; the CMake
project builds only the test executable `portable_file_watcher_tests`.

## Build, test, lint

```sh
# clangd and clang-tidy both need this compilation database:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
clang-tidy -p build portable_file_watcher_tests.cpp   # the CI lint step
```

Format code with `clang-format -i` (`.clang-format` is LLVM-based). There is no
other formatter/typecheck step.

## Layout & architecture

- `portable_file_watcher.hpp` — the entire library, split into two sections:
  - interface: types, the `PortableFileWatcher` class (member declarations
    only), and free-function declarations
  - a gated implementation section (`#ifdef PFW_IMPLEMENTATION`) with the
    out-of-line definitions, emitted in exactly one translation unit. When you
    edit a signature, update both sections; when you move a body, keep its
    NOLINTs. The impl block is wrapped in
    `NOLINTBEGIN/END(misc-definitions-in-headers)` — keep that range intact.
  Platform backends are chosen by preprocessor inside the header:
  - `_WIN32` → `ReadDirectoryChangesW` (supports recursive watching)
  - `__linux__` → `inotify`, a single watch on one directory; recursive is **not**
    implemented (no per-subdirectory `inotify_add_watch`)
  - `__APPLE__` / `__FreeBSD__` / `__OpenBSD__` / `__NetBSD__` → `kqueue` vnode
    watch (no child filenames, no recursion)
  - anything else → `#error`
- `portable_file_watcher_tests.cpp` — self-contained tests using a hand-rolled
  `CHECK` macro and `EventLog` helper; no gtest/Catch2. It defines
  `PFW_IMPLEMENTATION` before including the header (the repo's single
  implementation TU).
- CI (`.github/workflows/ci.yml`) builds on ubuntu/windows/macos, so platform code
  must compile on all three. Local dev on Linux can only compile the Linux branch.

## Gotchas

- Platform behavior intentionally diverges. `README.md` (§ Platform limitations)
  and the comment block at the top of the header are authoritative; when docs and
  header drift (e.g. `WatchedFileEvent` is `uint8_t` in code, `uint32_t` in the
  README), trust the header.
- The recursive-watch test asserts recursive delivery only on Windows; Linux
  (inotify) and macOS/BSD (kqueue) recursion is deliberately unasserted.
  `file_lifecycle` and `start_stop_repeatedly` assert child-filename events
  (Created/Modified/Renamed/Removed) only where the backend can report them —
  on kqueue they assert directory-level `Modified` events instead.
- Tests are event-driven and timing-sensitive: 5s `condition_variable` waits plus
  a `300ms` sleep for negative assertions. Re-run instead of assuming breakage;
  slow machines can be flaky.
- clang-tidy needs a real compilation database or it runs without flags and
  cascades hundreds of false positives (it defaulted to pre-C++17 in the
  Windows CI log: every `std::filesystem` use became an error and even
  correct code got "can be declared const"). CI therefore configures every OS
  with `-G Ninja` and `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` — the Visual Studio
  generator emits no database clang-tidy can load — and activates the MSVC
  environment (`ilammy/msvc-dev-cmd`) on Windows first so CMake picks `cl`
  rather than the MinGW gcc shipped on the runner image. macOS points `CC`/`CXX`
  at brew's LLVM clang (`$(brew --prefix llvm)/bin/clang++`): brew's clang-tidy
  cannot resolve the libc++ headers recorded by Xcode's clang in the database
  (the compile command carries no SDK sysroot), which used to collapse every
  standard header and flood the lint with cascades — `performance-enum-size`
  even flagged the enum despite its `std::uint8_t` base.
- `.clang-tidy` enables nearly all check groups with `WarningsAsErrors`, and
  `HeaderFilterRegex` means the header is analyzed too. Suppressions follow the
  `// NOLINTNEXTLINE(...)` convention already used throughout — match it.
  Notes on toolchain drift:
  - `misc-include-cleaner` cannot map user-defined literals such as `300ms` back
    to `<chrono>`, so the tests never use duration literals — write
    `std::chrono::milliseconds(300)` (with `readability-magic-numbers`
    suppressed when the value is not a power of two) instead.
  - Check behavior still differs between toolchains: local clang-tidy (LLVM 22)
    does not fire `misc-const-correctness` / `readability-implicit-bool-conversion`
    on the inotify branch the way CI's apt clang-tidy does, so run the exact CI
    lint step and search both old and new diagnostics.
- `.clangd` points at `build/` (`UnusedIncludes: Strict`) — configure the build
  before expecting clangd diagnostics to work.
- The header defines `NOMINMAX` before `#include <windows.h>`; keep it so the
  single header stays safe to include anywhere.
- `build/` is ignored; `.cache/` is a local clangd index, not tracked.
- On Windows, `start()` blocks until the worker's first (overlapped)
  `ReadDirectoryChangesW` is queued — `ReadDirectoryChangesW` only reports changes
  that occur after its first call, so without that handshake events immediately
  following `start()` are lost. Keep the no-read-in-flight-on-exit invariant when
  editing the worker loop: every exit path must have finished waiting on the
  pending read (`GetOverlappedResult`) before the buffer/completion event are
  destroyed. The event buffer is allocated in `start()` and moved into the
  worker (a `bad_alloc` must not escape the thread body), and everything after
  the readiness handshake is guarded by a `try/catch` that reports
  `WatchedFileEvent::Error` — keep the handshake (`ready.set_value`) outside
  that guard or an early exception would deadlock `start()`.