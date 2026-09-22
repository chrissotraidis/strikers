// The default host interface: four no-ops.
//
// A host application that draws its own controls over the game defines these instead, and a strong
// definition beats a weak one. Keeping the defaults here rather than behind an #ifdef means an
// ordinary desktop build links the same object list as a mobile one, so the absence of a host
// interface is a property of the host and never of the platform.

#include "port/hostui.h"
#include <stdio.h>
#include <time.h>

// These must lose to a host's own definitions; a strong symbol beats a weak one, and
// `weak_definition` on Mach-O and `weak` on ELF are the same declaration in clang.
//
// Losing is automatic only when the host's definitions are themselves on the link line. A weak
// definition in this object file already satisfies every reference, so the linker has no reason to
// search an archive for these symbols and a host that packages its side as a static library is
// never reached -- the build links, runs, and quietly has no host interface. A host packaging them
// that way must pull its archive in unconditionally; Ballpad's iOS app pairs a whole-archive pull
// with an explicit -Wl,-u for each hook.
#if defined(__GNUC__)
#define PORT_WEAK __attribute__((weak))
#else
#define PORT_WEAK
#endif

PORT_WEAK void PortHostUIStart(void* sdlWindow)
{
    (void)sdlWindow;
}

PORT_WEAK void PortHostUIFrame(void)
{
}

// A host with no interface of its own never wants the game held still.
PORT_WEAK int PortHostUIWantsPause(void)
{
    return 0;
}

// The default is the sleep the loop would otherwise have taken. A host that shares this thread with
// a user interface replaces it; nothing on the desktop does, and nothing there needs to.
PORT_WEAK void PortHostUIIdle(double seconds)
{
    if (seconds <= 0.0)
        return;
    {
        struct timespec request;
        request.tv_sec = (time_t)seconds;
        request.tv_nsec = (long)((seconds - (double)request.tv_sec) * 1000000000.0);
        nanosleep(&request, NULL);
    }
}

PORT_WEAK void PortHostUIStop(void)
{
}

// "No host pad" is the whole answer on a platform with no host interface: the port's own synthetic
// input is then the only source, exactly as it was before this seam existed.
PORT_WEAK int PortHostUIPollPad(PortHostPad* out)
{
    (void)out;
    return 0;
}

// A host with no store of its own, and no importer: the port's own search and its own refusal box
// are then the whole answer, which is what a desktop build wants and what the N1/N3 proofs and the
// scenario runner's STRIKERS_DATA route depend on.
PORT_WEAK const char* PortHostUIGameDataPath(void)
{
    return NULL;
}

PORT_WEAK int PortHostUIRunGameDataImport(const char* title, const char* message)
{
    (void)title;
    (void)message;
    return 0;
}

PORT_WEAK void PortHostUIDiagnostic(const char* message)
{
    fprintf(stderr, "[port] %s\n", message != NULL ? message : "");
}
