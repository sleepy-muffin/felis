# felis

A modern C++ utility library for reusable components across media processing, data handling, and system-level applications.

## Features

- Modern C++20 codebase
- Reusable media and data processing components
- Audio/image processing utilities
- Cross-platform design
- CMake-based integration
- Modular targets: link only the components you need


## Modules

Every component is a separate CMake target. Link `felis::<module>` and CMake
builds and links only that module and its dependencies.

| Target | Contents | External dependencies |
| --- | --- | --- |
| `felis::common` | Core: logging, strings, TOML, JSON, sync, async, math, legacy | - |
| `felis::external` | Boost.JSON file helpers | Boost (json) |
| `felis::sqlite3` | SQLite wrapper | SQLite3 |
| `felis::image` | Image/texture loading | OpenGL, JPEG, PNG |
| `felis::audio` | Audio encode/decode | opusenc, opusfile, mpg123, lame |

The standard-library-only components live in a single `felis::common`
target; their headers can still be included individually.

The `felis` target is an umbrella that links every enabled module. It is kept
for backwards compatibility; prefer linking `felis::<module>` directly.


## Usage

```cmake
add_subdirectory(felis)

target_link_libraries(app PRIVATE felis::common)
```

When felis is vendored via `add_subdirectory()` or `FetchContent`, module
targets are excluded from the default build, so only the modules your targets
actually link are compiled.

Modules with heavy dependencies can be disabled entirely. Their dependencies
are then not searched for at configure time:

```cmake
set(FELIS_BUILD_AUDIO OFF)
set(FELIS_BUILD_IMAGE OFF)
set(FELIS_BUILD_SQLITE3 OFF)
set(FELIS_BUILD_EXTERNAL OFF)

add_subdirectory(felis)
```

With `FetchContent`, set the same options before `FetchContent_MakeAvailable()`.

Building felis directly produces one static library per module, named
`felis-<module>-<platform>-<arch>`.
