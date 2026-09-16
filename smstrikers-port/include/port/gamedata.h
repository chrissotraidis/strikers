// Whether the port found a disc it can play, and, when it did not, the box it would have shown.
//
// The port resolves its disc from a static initialiser (nlMalloc -> nlInitMemory -> DVDInit), which
// on a mobile host runs before that host application's own framework exists. Exiting there would
// take a host that has an importer down with it, so a disc the port cannot use is recorded instead
// and the entry point decides what to do about it. See include/port/hostui.h for the two hooks a
// host uses to answer.

#ifndef PORT_GAMEDATA_H
#define PORT_GAMEDATA_H

#ifdef __cplusplus
extern "C" {
#endif

// Nonzero once a disc image or an extracted filesystem has been found, indexed, and identified as
// this game's disc. False after a refusal, even when files were found.
int PortGameDataReady(void);

// What the port would have shown. Both are never NULL, and both are empty until the first
// resolution has run.
const char* PortGameDataErrorTitle(void);
const char* PortGameDataErrorMessage(void);

// Resolve again, from the state the first attempt started from: called by the port's entry point
// after a host has staged the player's own disc, so that the entry point needs no knowledge of the
// DVD layer it is re-running. A no-op when the first attempt already found usable data.
void PortGameDataResolve(void);

#ifdef __cplusplus
}
#endif

#endif // PORT_GAMEDATA_H
