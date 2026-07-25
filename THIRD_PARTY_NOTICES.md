# Third-Party Notices — HyprFluidGlass

## OverShifted / LiquidGlass

Portions of the optical/refraction shader math in `src/main.cpp` — specifically the
`candidateFragmentSource()` LiquidGlass profiles (superellipse SDF, the nonlinear
`f(x) = 1 − b·(c·e)^(−d·x − a)` distance response, the centre-ward coordinate-contraction lens,
the screen-space grain, and the angular glow) — are adapted from:

- **Project:** OverShifted / LiquidGlass
- **Source:** https://github.com/OverShifted/LiquidGlass
- **Pinned revision:** `3797fa541c1f026c521a75885ee271f03bdf9f0f`
- **Adapted into:** `src/main.cpp` (`candidateFragmentSource`)

The upstream is a standalone OpenGL demo; only the optical math is adapted here, re-expressed on this
plugin's own Hyprland framebuffer capture, half-resolution blur, monitor-transform mapping, and
premultiplied-alpha compositing. No upstream showcase assets, background images, the `OverEngine`
submodule, or application/UI code were copied.

### Upstream license (MIT)

```
MIT License

Copyright (c) 2026 Sepehr Kalanaki

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
