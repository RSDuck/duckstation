## Building

Requires devkitA64, libnx and the Switch CMake package. Also a  bunch of portlibs package I forgot to write down. Install them as errors pop up.

```bash
$ /opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-cmake -G "Ninja" -DBUILD_NOGUI_FRONTEND=ON -DBUILD_QT_FRONTEND=OFF -DENABLE_OPENGL=OFF -DENABLE_VULKAN=OFF -DENABLE_CUBEB=OFF -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON  ../..
```
