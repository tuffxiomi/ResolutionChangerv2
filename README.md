# ResolutionChanger

A small native Android mod for LeviLauncher/Preloader that scales Minecraft Bedrock's rendering buffer resolution for optimization.

## Configuration

The Mod Menu contains exactly two controls:

- **Resolution** — slider from **25% to 100%**
- **Apply** — applies the selected percentage

There are no Width, Height, or Enabled settings.

The selected percentage is also saved to `config/config.json`.

Example:

```json
{
  "resolution": 75
}
```

## How it works

ResolutionChanger does not include BedrockTools files. It uses the Preloader hook API and Android EGL/native-window path:

- `eglCreateWindowSurface` captures the native window.
- `eglSwapBuffers` re-applies the selected percentage when needed.
- `eglDestroySurface` releases the captured native window.

The percentage scales the current surface dimensions, preserving the aspect ratio instead of requiring separate Width/Height values.

## Compatibility

- Android native
- ARM64 (`arm64-v8a`)
- ARMv7 (`armeabi-v7a`)
- C++20
- Preloader Android SDK
- LeviLauncher native mods
