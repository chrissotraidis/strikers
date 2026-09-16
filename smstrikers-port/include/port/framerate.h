// What the frame limiter caps to: vi.c sleeps to an absolute deadline in place of the console's 60
// Hz field rate, and the period comes from the display main() found the window on.

// With vsync on the limiter is held slightly above the refresh rate, because two pacers in series
// at the same rate but a different phase drift, and each time the deadline lands just after a
// vblank the frame waits nearly a whole extra period.
//
// The derived rate is capped at the game's own NTSC field (vi.c's VI_FIELD_NS), because the period
// is also the game's clock: one VIWaitForRetrace is one game frame, and the whole tree is locked to
// it. A rate above the field -- a 1.05 margin over a 60 Hz display is 63 Hz -- does not buy frames,
// it buys speed, and the audio transport, drained in real time at 5 ms a tick, cannot follow a
// clock that is running fast.

// STRIKERS_FPS_LIMIT overrides, in Hz or 0 for unlimited, and is taken exactly.

#ifndef PORT_FRAMERATE_H
#define PORT_FRAMERATE_H

#ifdef __cplusplus
extern "C" {
#endif

// Tell the limiter what the display does: `hz` is the refresh rate, 0 when unknown (outside
// 20..2000 Hz counts as unknown), `vsync` non-zero when present paces too. Each call re-derives the
// period.
void PortSetDisplayRefresh(double hz, int vsync);

// A rate in Hz, 0 for uncapped, or negative to follow STRIKERS_FPS_LIMIT again.
void PortSetFrameLimit(double hz);

void PortFrameLimitInfo(double* limitHz, double* displayHz, int* vsync, int* overridden);

#ifdef __cplusplus
}
#endif

#endif // PORT_FRAMERATE_H
