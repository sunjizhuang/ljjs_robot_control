# Mosquitto + OpenSSL 1.1 runtime libraries

This project packages the official Ubuntu 20.04/Focal Mosquitto 1.6.9 runtime
and its OpenSSL 1.1 runtime dependencies by target architecture.

```text
lib/x86_64/
  libmosquitto.so*
  libssl.so.1.1
  libcrypto.so.1.1

lib/aarch64/
  libmosquitto.so*
  libssl.so.1.1
  libcrypto.so.1.1
```

The `fetch_mosquitto_ubuntu20.sh` script also puts the matching official
Mosquitto broker under `bin/<arch>/` and `mosquitto.h` under `include/`.

No host-wide installation is required for the link step. The ARM64 build
continues to use the existing `aarch64-linux-gnu-g++` selection in the project;
no separate CMake toolchain file is introduced.
