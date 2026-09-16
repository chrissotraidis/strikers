
#ifndef PORT_INPUT_H
#define PORT_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

int PortInputPadSetting(unsigned int pad);
int PortInputKeyboardEnabled(void);

int  PortInputRecordStart(const char* path);
void PortInputRecordStop(void);
const char* PortInputRecordPath(void);

// A recording's frame numbers are absolute, so one started here is shifted.
int  PortInputReplayStart(const char* path);
void PortInputReplayStop(void);
int  PortInputReplayStatus(unsigned long* count, unsigned long* cursor,
                           unsigned long* firstFrame, unsigned long* lastFrame,
                           const char** path);

// Hold a PAD_BUTTON_* mask on pad 0, merged with the real pad; 0 frames is the default hold.
void PortInputPress(unsigned int buttons, int frames);

// The same for the main stick, at -100..100 per axis; nothing in the environment drives it, so it
// exists for the control channel's `stick`.
void PortInputStick(int x, int y, int frames);

// One of STRIKERS_AUTOPRESS's button names as a mask for the call above, or 0; `len` bounds the
// name, and negative means to the NUL.
unsigned int PortInputButtonFromName(const char* name, int len);

unsigned long PortInputFrame(void);

// A host interface's controller-mapping row. The physical map is Aurora's and is chosen at startup
// from STRIKERS_PAD_*, so there is no runtime remap store to route a menu row into; what a row can
// honestly do is show the person the map their device actually has, read back from the same place
// `report_gamepad` prints it, without the app being linked against the engine's pad header to do
// it. Rows are rebuilt on every call: connecting a controller reloads the whole table.
typedef struct PortPadRow
{
    const char* pad;      // the GameCube control: "A", "Z", "Start", "Stick X"
    const char* native;   // what it is bound to here: "south", "+leftx", "(none)"
} PortPadRow;

// Fills `*rows` with the current map for `port` and returns its length; 0 with `*rows` set to NULL
// means nothing is on that port. Both strings belong to the mapping table and stay valid only until
// the next call.
int PortPadMapping(unsigned int port, const PortPadRow** rows);

// The device name, or NULL when the port is empty.
const char* PortPadName(unsigned int port);

// Dead zones, trigger mode and rumble as one line, or NULL when the port is empty. Static storage,
// same lifetime rule as the rows above.
const char* PortPadDetail(unsigned int port);

// What the engine's own pad holds for a port: PadStatus::s_Current[port], the sample the game's
// tasks read through cPlatPad (cPlatPad::IsPressed and friends), after VBlankPadUpdate has merged
// every source into it. A host's offer (PortHostUIPollPad, port/hostui.h) is a request; this is the
// answer, one pad-assembly boundary later. The two are different claims, and only the second one
// says the game read a control rather than that an overlay drew one.
//
// The bits are the same spelling a host publishes, so a caller can compare an offer and a reading
// without a translation table: PAD_BUTTON_LEFT 0x0001 .. PAD_BUTTON_START 0x1000, PAD_TRIGGER_Z
// 0x0010, PAD_TRIGGER_R 0x0020, PAD_TRIGGER_L 0x0040. The sticks are -128..127 with -128 as the
// pad's own "nothing here" reading, and the triggers are 0..255.
typedef struct PortPadEngineState
{
    int          err;           // PAD_ERR_*; cPlatPad::IsConnected reads 0 and -3 as connected
    unsigned int buttons;
    int          stickX, stickY;
    int          substickX, substickY;
    int          triggerLeft, triggerRight;
} PortPadEngineState;

// Returns 1 and fills *out for ports 0..3, and 0 for any other port or a NULL out, leaving *out
// zeroed. It reports the pad as it stands rather than a fabricated neutral, so an empty port reads
// back as a nonzero err.
int PortPadEngineRead(unsigned int port, PortPadEngineState* out);

#ifdef __cplusplus
}
#endif

#endif // PORT_INPUT_H
