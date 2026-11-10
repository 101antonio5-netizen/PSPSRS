# PSPSRS

PSPSRS aims to create a spaced repetition program for the PSP, using PSPSDK and its development environment.

## Building

Install and configure [PSPSDK](https://pspdev.github.io/) and its development environment, including the `psp-cmake` command. From the repository root, run:

```bash
mkdir build && cd build
psp-cmake ..
make
```

This configures and builds the project. The resulting `eboot.pbp` is generated in the build directory.
