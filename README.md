# glide3xvk — Glide 3 → Vulkan Wrapper for Diablo II LoD

A drop-in `glide3x.dll` replacement that renders Diablo II 1.14d through Vulkan using a CPU-based rasterizer.

## Status
✅ **Working**: Full Glide 3 API, Vulkan backend, CPU rasterizer, shader presets  
❌ **Removed**: GPU rasterizer (Route B) - removed for stability  
📊 **Performance**: ~18 FPS at 1080p  
🎮 **Tested**: Vanilla retail 1.14d installs with Vulkan GPUs

## Quick Start

1. **Requirements**:
   - Diablo II LoD 1.14d (retail, no rebuild needed)
   - Vulkan GPU with current drivers
   - MSYS2 MINGW32 toolchain

2. **Build**:
   ```bash
   cmake -S . -B build-g3x -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake
   cmake --build build-g3x --target glide3x
   ```

3. **Install**:
   - Copy `glide3x.dll` to your Diablo II game directory
   - Copy `d2slangvk.conf.sample` to `d2slangvk.conf` (optional)

4. **Run**:
   ```bash
   Game.exe -3dfx
   ```

## Features

- **Complete Glide 3 API**: 45 stdcall exports, bit-exact rasterization
- **Vulkan Backend**: Hardware acceleration, swapchain management
- **Shader Presets**: Post-processing chain via `d2slangvk.conf`
- **Overlay System**: Version info, FPS counter, hotkey status (Scroll Lock toggle)
- **Controller Support**: Experimental gamepad mapping
- **Screenshots**: PrintScreen support

## Configuration

Edit `d2slangvk.conf`:
- `preset=` - Shader preset chain
- `overlay=1` - Enable overlay
- `controller=1` - Enable controller support
- `window=` - Window size preset

## Hotkeys

- **Scroll Lock**: Toggle overlay
- **F11**: Cycle aspect ratio
- **F12**: Cycle shader resolution
- **Shift+V**: Configuration panel

## Architecture

```
Game.exe → glide3x.dll → CPU Rasterizer → Vulkan Swapchain
                                    ↓
                              Shader Presets (Optional)
```

## Documentation

- [FIELD-TEST.md](FIELD-TEST.md) - Field testing results
- [BUILD-32BIT.md](BUILD-32BIT.md) - Build instructions
- [docs/CURRENT-STATUS.md](docs/CURRENT-STATUS.md) - Current project status

## License

MIT License - see LICENSE file for details.