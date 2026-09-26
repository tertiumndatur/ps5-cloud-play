# Notices

The original application and PS5 scaffold files are GPL-3.0-or-later; bundled
Chiaki Cloud files retain their own license. The PS5 native build scaffold, runtime
shim, SDL integration archives, socket adapter, controller input, and keyboard
code were adapted from ProsperoLight, whose source files carry their own
copyright and SPDX headers. The public PS5 payload SDK is fetched by the
build scripts and is not distributed in this repository. No Sony SDK, account
token, or proprietary game file is included.

`third_party/nlohmann/json.hpp` is nlohmann/json by Niels Lohmann and
contributors, under the MIT license embedded at the top of the header.

The `third_party/chiaki-cloud` source is copied from the local chiaki-cloud
checkout and retains its AGPL-3.0-only-OpenSSL license and bundled dependency
licenses under `third_party/chiaki-cloud/LICENSES` and the dependency trees.
The application links its catalog, provisioning, streaming, Opus, and FEC
components. Video is decoded by the PS5 VideoDec2 system module and presented
through the ProsperoLight AGC zero-copy path. The build also links PacBrew
libcurl, OpenSSL, libevent, libiconv, and their transitive libraries from the pinned PacBrew sysroot.
Their license files are provided by that upstream distribution.

The launcher interface incorporates the Paper Library design language, drawing
code, font format, controller glyphs, animation helpers, and PS5 EGL platform
code from
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui),
Copyright (C) 2026 BlackBearReloaded, under GPL-3.0-or-later. Its license and
third-party notices are retained under `third_party/ps5-homebrew-ui/`.

The PS5 build statically links the pinned
[ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) SDK
(GPL-3.0-or-later), which includes Mesa under the MIT license and OpenGNM PSBC
under its upstream terms. The SDK is checksum verified into the ignored
`.deps/ps5-opengl/` directory and is not stored in this repository.

The baked Inter Regular, Inter SemiBold, and Montserrat Medium font assets are
distributed under the SIL Open Font License 1.1. Their license texts ship next
to the fonts in `assets/fonts/`.

`third_party/tlsf` is TLSF 3.1 by Matthew Conte, pinned from upstream commit
`deff9ab509341f264addbd3c8ada533678591905`, under the BSD license included
in the headers of `third_party/tlsf/tlsf.c` and `third_party/tlsf/tlsf.h`.

`src/qr/qrcodegen.c` and `src/qr/qrcodegen.h` are Project Nayuki's QR Code
generator library under the MIT license embedded in both source files.

`third_party/json-c-0.19` is json-c 0.19 from the official release tag
`json-c-0.19-20260627`, under the MIT license in its `COPYING` file. The PS5
build disables locale switching and thread-local storage, which the public
payload runtime does not fully support. The JSON object hash seed mixes the
monotonic clock and a process address on PS5.

`tools/build-emutls.sh` extracts Clang 18 compiler-rt's `emutls.c.o` from the
installed LLVM runtime to satisfy PacBrew's emulated thread-local storage.
The compiler-rt runtime is provided under the Apache License 2.0 with LLVM
Exceptions. It is not stored in the source tree.

`assets/ca-certificates.crt` is the public CA trust bundle from Ubuntu's
`ca-certificates` package (20260601~24.04.1). It is included so that PS5
libcurl can verify HTTPS endpoints; it contains no private key.
