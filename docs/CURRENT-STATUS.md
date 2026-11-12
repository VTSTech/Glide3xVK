# CURRENT STATUS - CPU Rasterizer Only

## Overview
As of commit 967a34d (0.4.8), Glide3xVK is using a **CPU-based rasterizer** only. The GPU rasterizer (Route B) has been removed from the codebase to ensure stability.

## What's Working
- ✅ Full Glide 3 API implementation (45 stdcall exports)
- ✅ Vulkan backend with swapchain management
- ✅ CPU scanline rasterizer
- ✅ Palette texture handling
- ✅ Chroma key transparency
- ✅ LFB (Linear Frame Buffer) support
- ✅ Overlay system (Scroll Lock toggle)
- ✅ Screenshot functionality (PrintScreen)
- ✅ libslang-vk preset chain integration

## What's Not Implemented
- ❌ GPU rasterizer (Route B) - removed for stability
- ❌ Bilinear texture filtering (point sampling only)
- ❌ Advanced shader parameters UI

## Performance
- Typical FPS: 18 FPS at 1080p resolution
- CPU rasterizer is stable and reliable
- No shader corruption issues

## Build Requirements
- MSYS2 MINGW32 toolchain
- Vulkan SDK
- libslang-vk dependency

## Configuration
The configuration is handled through `d2slangvk.conf`. Key settings:
- `overlay=1` - Enable overlay display
- `preset=` - Specify shader preset chain
- `controller=1` - Enable controller support

## Field Testing
Tested on vanilla Diablo II LoD 1.14d retail installs with Vulkan-capable GPUs.