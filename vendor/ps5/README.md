# PS5 source dependencies

This directory contains source-only link stubs for PS5 system modules that are
not present in the public payload SDK, plus the SDL2 public headers.

SDL2 and the C++ runtime archives are restored into the ignored `.deps/` cache
from pinned public PS5 Payload SDK and PacBrew releases during the build. No
generated `.a` or `.prx` file is kept in the source tree.

The launcher itself uses ps5-homebrew-ui and ps5-opengl; their code, assets,
and notices live outside this vendor directory.
