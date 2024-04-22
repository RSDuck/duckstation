## Installation

To use it put the switch folder from the 7z file onto the root of your SD card, so that there is a `duckstation` folder inside the `switch` folder.

Alternatively you can also put the duckstation folder wherever you like.

## Building

Requires devkitA64, libnx and the Switch CMake package. Also a  bunch of portlibs package I forgot to write down. Install them as errors pop up.

```bash
$ /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -G "Ninja" -DBUILD_NOGUI_FRONTEND=ON -DBUILD_QT_FRONTEND=OFF -DENABLE_OPENGL=OFF -DENABLE_VULKAN=OFF -DENABLE_CUBEB=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON  ../..
```
