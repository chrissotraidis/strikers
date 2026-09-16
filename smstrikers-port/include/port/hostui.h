#ifndef PORT_HOSTUI_H
#define PORT_HOSTUI_H

// The port's one door into the host application's own user interface.
//
// The port owns the game, the window and the frame loop, so it is the only place that knows when
// the window exists and when a frame's input has finished being assembled. An Android, iOS or
// web host wants to put its own interface above the renderer; on the desktop there is nothing to
// put there. These four hooks are that seam, and they are deliberately opaque: the port passes a
// window it does not describe, and the host decides what to do with it.
//
// Defaults are weak no-ops (src/platform/hostui.c), so a build that supplies no host interface
// still links and runs. A host that supplies strong definitions replaces them.
//
// Order the port guarantees:
//   PortHostUIStart    once, after the window exists and before the first frame.
//   PortHostUIPollPad  once per frame, while the port assembles that frame's pad and before the
//                      game's tasks read it. This is where a host's input joins the port's own.
//   PortHostUIFrame    once per frame, after that pad is assembled: a host's own per-frame
//                      interface work (relayout, controller visibility) belongs here.
//   PortHostUIStop     once, after the frame loop and before the window is destroyed.
//
// On iOS the whole loop runs on the main thread, so these are main-thread calls and a host may
// touch UIKit from them without hopping.

// The pad a host publishes. Its bits are the GameCube pad's, spelled here so that a host can
// translate its own layout without including a Dolphin header; the port asserts them against
// dolphin/pad.h's PAD_BUTTON_*/PAD_TRIGGER_* at compile time (src/platform/input.cpp), so the two
// spellings cannot drift apart without a build failure. PAD_BUTTON_MENU shares the START bit and
// is deliberately not a second name here.
enum
{
    PORT_PAD_BUTTON_LEFT = 0x0001,
    PORT_PAD_BUTTON_RIGHT = 0x0002,
    PORT_PAD_BUTTON_DOWN = 0x0004,
    PORT_PAD_BUTTON_UP = 0x0008,
    PORT_PAD_TRIGGER_Z = 0x0010,
    PORT_PAD_TRIGGER_R = 0x0020,
    PORT_PAD_TRIGGER_L = 0x0040,
    PORT_PAD_BUTTON_A = 0x0100,
    PORT_PAD_BUTTON_B = 0x0200,
    PORT_PAD_BUTTON_X = 0x0400,
    PORT_PAD_BUTTON_Y = 0x0800,
    PORT_PAD_BUTTON_START = 0x1000
};

// One frame of host input, in the port's own scale: the main stick and the C stick are -127..127,
// and a trigger is 0..255.
typedef struct PortHostPad
{
    unsigned int buttons;   // PORT_PAD_* bits
    int stickX;
    int stickY;
    int substickX;
    int substickY;
    int triggerLeft;
    int triggerRight;
} PortHostPad;

#ifdef __cplusplus
extern "C" {
#endif

// `sdlWindow` is the SDL_Window* the port presents into, or NULL if there is none.
void PortHostUIStart(void* sdlWindow);

// The host's pad for the current frame. The port merges it with its own synthetic input -- buttons
// are OR'ed, the axis with the larger magnitude wins, a trigger takes the larger pressure -- rather
// than letting either side replace the other, so a host's touch controls and the port's own
// keyboard/AUTOPRESS pad stay usable at the same time. Returns nonzero when the host has a pad at
// all, which says the host is present rather than that any control is held.
//
// Called exactly once per frame. A host whose input source latches rising edges and clears them
// when read must consume that source once per call, or a press survives into an unrelated frame.
int PortHostUIPollPad(PortHostPad* out);

void PortHostUIFrame(void);

void PortHostUIStop(void);

// Game data: the disc the port is going to play, for a host that has to import it.
//
// The port resolves its disc before the host's own framework exists -- from a static initialiser,
// ahead of main() -- and looks only in the environment, the directory beside the executable and the
// working directory. A sandboxed host application can write to none of those. These two hooks are
// how a host's own store joins that search, and they are the pair that makes an import survive a
// cold start rather than only the run that performed it.
//
// Order the port guarantees:
//   PortHostUIGameDataPath      from the port's disc search, which is a static initialiser. The
//                               host must not touch a UI framework from here.
//   PortHostUIRunGameDataImport from the port's entry point, after a search that found nothing
//                               usable and before any game code reads the disc. On iOS this is
//                               inside the host's application delegate, so UIKit is up.

// The host's location for the disc, or NULL when it has staged nothing. A disc image or an
// extracted `files` directory, in any format port_disc_open accepts (include/port/disc.h). The
// port copies it, so the returned pointer need not outlive the call.
const char* PortHostUIGameDataPath(void);

// The port's fatal `game data` / `unusable disc` refusal, handed to a host that would rather show
// its own importer. `message` is the finished, user-facing explanation the port would have put in
// the box, including its own wording for a refusal made after finding files. Returns nonzero if the
// host staged data and the port should resolve again, zero to let the port report the failure the way
// it always has.
int PortHostUIRunGameDataImport(const char* title, const char* message);

#ifdef __cplusplus
}
#endif

#endif // PORT_HOSTUI_H
