
#ifndef PORT_ASPECT_H
#define PORT_ASPECT_H

#ifdef __cplusplus
extern "C" {
#endif
float PortTargetAspect(void);

// Pin the display aspect at runtime, or restore following the window with a value <= 0. A host
// interface's own aspect control calls this; it bumps PortAspectGeneration, which main()'s frame
// loop turns into PortApplyAspectChange and a frame-buffer resize on the next frame, so the game's
// render mode, locked aspect and front end all move together rather than only the picture's shape.
void PortSetTargetAspect(float aspect);

// Non-zero while the aspect is the window's own rather than a pinned value.
int PortAspectFollowsWindow(void);

// The width of the logical framebuffer, in the coordinate space the game's GXSetViewport,
// GXSetScissor and 2D orthographic projections all use. 640 at 4:3, 854 at 16:9.
unsigned int PortLogicalFrameWidth(void);

// Tell the aspect code what shape the window is, for STRIKERS_ASPECT=auto. main() calls this with
// the size Aurora reports from aurora_initialize, before glplatStartup runs and asks for a width,
// and then once a frame with the window's live shape.
void PortSetWindowAspect(unsigned int width, unsigned int height);

// Bumped whenever the effective aspect changes; 0 until it first does.
unsigned int PortAspectGeneration(void);

// Re-derive what is sized from the display aspect once rather than per frame: the render mode's
// frame width, the locked display aspect, and the screen width the front end anchors against.
void PortApplyAspectChange(void);

// How far to carry the gameplay camera from its 4:3 tuning toward its widescreen one. 0 at 4:3, 1
// at 16:9, and continuing linearly beyond.
float PortCameraAspectBlend(void);

// The (fov, ratio) pair to build a perspective projection with, given the pair the shipped game
// asked for.
void PortPerspective(float consoleFov, float consoleRatio,
                     float* outFov, float* outRatio);

// Whether to draw the 38-unit cinematic letterbox bars over a cutscene.
int PortDrawCinematicBars(void);
float PortGetFrustumStretch(void);
void  PortSetFrustumStretch(float stretch);       // [0,1]
float PortGetCameraBlendOverride(void);           // < 0 means derived
void  PortSetCameraBlendOverride(float blend);    // < 0 restores derived
void  PortSetDrawCinematicBars(int draw);

#ifdef __cplusplus
}
#endif

#endif // PORT_ASPECT_H
