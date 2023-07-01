# Introduction

Crosswalk is an app runtime based on Chromium/Blink.

### Ubuntu 18.04 is recommended for building xwalk.

### Steps

git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH=/home/[user]/depot_tools:$PATH
export XWALK_OS_ANDROID=1
gclient config --name=src/xwalk https://github.com/ks32/crosswalk.git@origin/ks_chromium_77

Edit this newly-created .gclient file, and add the following to the bottom:
target_os = ['android']

gclient sync

gn args out/Default_x64  
(add arguments as in args_x64.txt)

ninja -k3 -C out/Default_x64 xwalk_core_library

(If build failed, try again)

# Whats new 77.1.4.0

Remove xwalk library update/download code for complince with Google Play policy

fixes few build errors
