// The AuroraConfig fields that have to be chosen before the window exists.

#ifndef PORT_LAUNCH_H
#define PORT_LAUNCH_H

#if defined(PORT_USE_AURORA)

#include <aurora/aurora.h>

#ifdef __cplusplus
extern "C" {
#endif

// Apply STRIKERS_FULLSCREEN, STRIKERS_WINDOW_SIZE, STRIKERS_PAUSE_ON_FOCUS_LOST and STRIKERS_CACHE_DIR
// to `cfg`, give it a window icon, and place the window with its title bar on screen.
void PortAuroraConfigure(AuroraConfig* cfg);

void PortFollowRenderScale(unsigned int windowHeight);

// Pin the render scale, or hand the choice back to the window with a value <= 0. A host
// interface's own resolution rows call this; STRIKERS_RES_SCALE is the same pin at startup, which
// is why a mid-run change and an environment change land in one place.
void PortSetRenderScale(float scale);

float PortRenderScale(void);

// The render target the renderer is configured with, in pixels: the number next to the scale in the
// port's own `[port] render target` line, and the size Aurora hands the swapchain. Read back rather
// than recomputed by a caller, so "the setting reached the renderer" is a measurement.
void PortRenderTargetSize(unsigned int* width, unsigned int* height);

// Non-zero once per render-target size change, and clears that: the frame loop's signal that the
// swapchain has to be resized. A render scale change moves the target without moving the window,
// so Aurora's own window-resize path never sees it; this is what makes a runtime scale change land
// on the frame it was asked for rather than on whichever event happens to follow.
int PortTakeRenderTargetChange(void);

#ifdef __cplusplus
}
#endif

#endif // PORT_USE_AURORA

#endif // PORT_LAUNCH_H
