# Building and running

## Dependencies

* CMake 3.28 or newer, and Ninja
* A C++23 compiler: GCC 13 or newer, Clang 17 or newer
* Boost 1.70 or newer, with its CMake config files, and the `coroutine`,
  `log`, `log_setup`, `program_options` and `iostreams` components
* OpenSSL 1.1 or newer
* Boost.test and libcurl, for the test suites

There are no submodules and nothing is vendored.

## Building

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests are configured only when both Boost.test and libcurl are found. When
one of them is missing they are skipped without failing the configure step, so
check `ctest -N` if a test run reports nothing to do.

### CMake options

| Option | Default | What it does |
|---|---|---|
| `ENABLE_TESTS` | `ON` | Build `dracon_library_tests` and `dracon_server_tests` |
| `ENABLE_SANITIZERS` | `ON` in Debug | UndefinedBehaviorSanitizer and LeakSanitizer |
| `ENABLE_TRACE_LOG` | `OFF` | Compile the `TRACE()` log macro in |
| `ENABLE_DEBUG_LOG` | `OFF` | Compile the `DEBUG()` log macro in |

AddressSanitizer is deliberately not among them. It does not survive the stack
switching of Boost.Coroutine2, which is what the server runs its sessions on.

The hardening flags (`-fstack-protector-strong`, `-fstack-clash-protection`,
`_FORTIFY_SOURCE=2` outside Debug, full RELRO, PIE) are applied to every build
from the top level `CMakeLists.txt` and are not optional.

## Running from the build tree

The build tree has the shape of an install prefix, and the server resolves its
paths relative to its own binary, so it runs from there with no arguments:

```
build/bin/dracon                the server
build/lib/dracon/plugins/       the plugins it loads at start up
build/etc/dracon/               server.conf and the files it includes
```

```sh
build/bin/dracon                # http://localhost:8080, https://localhost:8443
```

The configuration in `build/etc/dracon/` is copied from `conf/` when CMake
configures the project. Edit the files in `conf/` and re-run CMake, otherwise
the next configure overwrites the copies.

## Command line

| Option | Meaning |
|---|---|
| `-c`, `--conf` | Configuration directory, `../etc/dracon` next to the binary by default |
| `-d`, `--plugins-dir` | Plugins directory, `../lib/dracon/plugins` by default |
| `-w`, `--workers` | Event loop threads, one per hardware thread by default |
| `-u`, `--user` | User to drop privileges to, overriding `server.conf` |
| `-g`, `--group` | Group to drop privileges to. Defaults to the main group of the user |
| `--pid` | Print the pid on stdout once the server is up |
| `-h`, `--help` | Print the options |

The command line wins over the `privileges` section of `server.conf`.

Dropping privileges only happens when the server is started as root. The
shipped systemd unit starts it as the `dracon` user with
`AmbientCapabilities=CAP_NET_BIND_SERVICE`, so it is never root, nothing is
dropped, and `--user` has nothing to do. The options are for the deployments
where nothing set the user up first: a container whose entrypoint runs as root,
a sysvinit script, or starting the server by hand to bind port 80.

## Installing

```sh
cmake --install build --prefix /usr/local
```

That installs the server, the bundled plugins, the configuration, the public
headers, and the CMake package. A plugin project then finds Dracon with

```cmake
find_package(Dracon REQUIRED)
target_link_libraries(MyPlugin PRIVATE dracon::dracon)
```

The package is `Dracon` and the target is `dracon::dracon`. Looking for
`dracon` in lower case never finds the installed package.

`debian/` builds a Debian package. `Dockerfile` builds and tests the server
inside Debian trixie, which is the oldest distribution Dracon targets, and is
the quickest way to check a change against the versions of GCC, CMake and Boost
that ship there:

```sh
docker build -t dracon .
```
