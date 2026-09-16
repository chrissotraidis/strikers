// DVD backed by an extracted disc filesystem, or by a disc image.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "dolphin/types.h"
#include "port/config.h"
#include "port/disc.h"
#include "port/gamedata.h"
#include "port/host.h"
#include "port/disc_reader.h"
#include "port/hostui.h"
#include "port/region.h"

void OSReport(const char* msg, ...);

// Defined below; DVDInit uses it to check that the directory it found is this game's disc rather
// than some other directory that happens to have files in it.
s32 DVDConvertPathToEntrynum(const char* pathPtr);

// Defined near DVDGetCurrentDiskID, below.
static void load_disk_id(void);
static const u8* disk_id_bytes(void);

// MSVC's <sys/stat.h> has the S_IFDIR bit but not the POSIX test macro.
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

// The 32-byte disc header: game code (4), maker (2), disk/version, flags, and the game name.
static u8 s_disk_id[32];
// Whether that read succeeded, so the fallback below is a decision and not an accident.
static int s_disk_id_read;
// Whether the id was chosen rather than read, for the DVD: disc line.
static int s_disk_id_guessed;

// The caches every resolver downstream of the disc table fills in, file-scope so that dvd_forget()
// can clear them: after a host import the disc is a different disc, and an answer derived from the
// refused attempt would be about data that is no longer there.
static int s_disk_id_loaded;      // load_disk_id
static int s_region_cache = -1;   // port_region
static char s_game_code[5];       // port_disc_game_code
static char s_maker_code[3];      // port_disc_maker_code

typedef struct DVDCommandBlock DVDCommandBlock;
typedef void (*DVDCBCallback)(s32 result, DVDCommandBlock* block);

struct DVDCommandBlock
{
    DVDCommandBlock* next;
    DVDCommandBlock* prev;
    u32 command;
    s32 state;
    u32 offset;
    u32 length;
    void* addr;
    u32 currTransferSize;
    u32 transferredSize;
    void* id;
    DVDCBCallback callback;
    void* userData;
};

typedef struct DVDFileInfo DVDFileInfo;
typedef void (*DVDCallback)(s32 result, DVDFileInfo* fileInfo);

struct DVDFileInfo
{
    DVDCommandBlock cb;
    u32 startAddr;
    u32 length;
    DVDCallback callback;
};

#define DVD_STATE_END 0
#define DVD_STATE_BUSY 1
#define DVD_STATE_FATAL_ERROR (-1)

typedef struct
{
    char* path;   // path as the game spells it, lowercased, '/'-separated
    char* host;   // full host path, or NULL when the bytes are in an image
    u32 offset;   // byte offset into the image; unused for a host file
    u32 length;
} DvdEntry;

static DvdEntry* s_entries;
static int s_count;
static int s_cap;
static char s_root[1024];
// Non-NULL when the data is a disc image rather than a directory.
static PortDisc* s_disc;

static char* dup_lower(const char* s)
{
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    for (size_t i = 0; i < n; i++)
        out[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] - 'A' + 'a' : s[i]);
    out[n] = '\0';
    return out;
}

static void add_entry(const char* rel, const char* host, u32 offset, u32 length)
{
    if (s_count == s_cap)
    {
        s_cap = s_cap ? s_cap * 2 : 256;
        s_entries = (DvdEntry*)realloc(s_entries, (size_t)s_cap * sizeof(DvdEntry));
    }
    s_entries[s_count].path = dup_lower(rel);
    s_entries[s_count].host = host != NULL ? strdup(host) : NULL;
    s_entries[s_count].offset = offset;
    s_entries[s_count].length = length;
    s_count++;
}

// Directory iteration goes through port_scan_dir: <dirent.h> is POSIX and Windows has none.
static void scan(const char* hostDir, const char* relDir);

struct ScanCtx
{
    const char* hostDir;
    const char* relDir;
};

static void visit_entry(void* user, const char* name)
{
    const struct ScanCtx* ctx = (const struct ScanCtx*)user;
    char host[1024], rel[1024];
    struct stat st;

    // Skip every dotfile: an extracted disc copied about on macOS collects .DS_Store, and the
    // console's filesystem had no such thing, so anything starting with a dot is not disc content.
    if (name[0] == '.')
        return;

    snprintf(host, sizeof host, "%s/%s", ctx->hostDir, name);
    snprintf(rel, sizeof rel, "%s%s%s", ctx->relDir, *ctx->relDir ? "/" : "", name);
    if (stat(host, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode))
        scan(host, rel);
    else
        add_entry(rel, host, 0, (u32)st.st_size);
}

static void scan(const char* hostDir, const char* relDir)
{
    struct ScanCtx ctx;
    ctx.hostDir = hostDir;
    ctx.relDir = relDir;
    port_scan_dir(hostDir, visit_entry, &ctx);
}

// ── Refusal, not exit ────────────────────────────────────────────────────────────────────────
//
// DVDInit runs from a static initialiser, before a host application's own framework exists (see
// include/port/gamedata.h). Exiting there would take a host that can import the player's own disc
// down with it, so a disc the port cannot use is recorded and the port's entry point decides: the
// host's importer gets its chance first, and only if it declines does this become the same box, with
// the same text, that it always was.
static char s_refuseTitle[128];
static char s_refuseMessage[4096];
static int s_refused;

static void dvd_refuse(const char* title, const char* text)
{
    snprintf(s_refuseTitle, sizeof s_refuseTitle, "%s", title);
    snprintf(s_refuseMessage, sizeof s_refuseMessage, "%s", text);
    s_refused = 1;
}

int PortGameDataReady(void)
{
    return s_count != 0 && !s_refused;
}

const char* PortGameDataErrorTitle(void)
{
    return s_refuseTitle;
}

const char* PortGameDataErrorMessage(void)
{
    return s_refuseMessage;
}

// A fresh start: everything a resolution attempt built, and everything it derived, so the next
// attempt cannot be answered by the attempt that was refused. The table is malloc'ed rather than
// taken from the game heap, so this is safe whenever the host's import finishes, however late that
// is.
static void dvd_forget(void)
{
    int i;
    for (i = 0; i < s_count; i++)
    {
        free(s_entries[i].path);
        free(s_entries[i].host);
    }
    free(s_entries);
    s_entries = NULL;
    s_count = 0;
    s_cap = 0;
    s_root[0] = '\0';
    if (s_disc != NULL)
    {
        port_disc_close(s_disc);
        s_disc = NULL;
    }
    s_disk_id_read = 0;
    s_disk_id_guessed = 0;
    s_disk_id_loaded = 0;
    s_region_cache = -1;
    s_game_code[0] = '\0';
    s_maker_code[0] = '\0';
    s_refused = 0;
    s_refuseTitle[0] = '\0';
    s_refuseMessage[0] = '\0';
}

static void image_refuse(const char* path, const char* text)
{
    char msg[4096];
    snprintf(msg, sizeof msg, "%s\n\n  Image: %s\n", text, path);
    dvd_refuse("Super Mario Strikers: unusable disc image", msg);
}

static int index_visit(void* user, const char* path, unsigned offset, unsigned length,
                       int isDir)
{
    (void)user;
    if (!isDir)
        add_entry(path, NULL, offset, length);
    return 0;
}

// 0 when the image is indexed and usable, -1 when it has been refused with a reason.
static int index_image_fst(const char* path)
{
    char err[2048];

    // The disc id, from the image itself. An extracted folder reads these same 32 bytes out of
    // sys/boot.bin, which is a copy of them, so region handling is identical either way
    // (include/port/region.h).
    if (port_disc_read(s_disc, s_disk_id, sizeof s_disk_id, 0) != (long)sizeof s_disk_id)
    {
        image_refuse(path, "That image is too short to hold a disc header.");
        return -1;
    }
    s_disk_id_read = 1;

    if (port_disc_walk(s_disc, index_visit, NULL, err, sizeof err) != 0)
    {
        image_refuse(path, err);
        return -1;
    }
    return 0;
}

// Open `path` as a disc image and index it. 0 on success, -1 when refused.
static int open_image(const char* path)
{
    char err[2048];

    s_disc = port_disc_open(path, err, sizeof err);
    if (s_disc == NULL)
    {
        image_refuse(path, err);
        return -1;
    }

    snprintf(s_root, sizeof s_root, "%s", path);
    if (index_image_fst(path) != 0)
        return -1;

    // An image whose FST names no files would leave s_disc set and s_count 0, so DVDInit would scan
    // a directory into the same table, whose entries then carry host paths while reads take the
    // image branch.
    if (s_count == 0)
    {
        image_refuse(path, "That image contains no files at all.");
        return -1;
    }
    return 0;
}

// A path names an image if it is a file rather than a directory.
static int is_regular_file(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

// Pick a disc image out of the directory beside the executable, so dropping one next to the game is
// as good as unpacking a files/ folder there.
struct PickCtx
{
    const char* dir;
    char best[1024];
};

static void pick_image(void* user, const char* name)
{
    struct PickCtx* ctx = (struct PickCtx*)user;
    if (name[0] == '.' || !port_disc_looks_like_image(name))
        return;
    if (ctx->best[0] == '\0' || strcmpi(name, ctx->best) < 0)
        snprintf(ctx->best, sizeof ctx->best, "%s", name);
}

// One file that is on every copy of this disc and on nothing else a person is likely to point
// STRIKERS_DATA at.
#define DVD_SENTINEL "common.ini"

// The paths tried, in the order tried, so the message can name them.
#define DVD_MAX_TRIED 5

void DVDInit(void)
{
    char tried[DVD_MAX_TRIED][1024];
    const char* triedWhy[DVD_MAX_TRIED];
    int nTried = 0;

    // A second call means a host supplied data after an attempt was refused, so start from the state
    // the first attempt started from rather than from a half-built table: a folder of the wrong game
    // scans into that table before it is refused.
    if (s_refused)
        dvd_forget();

    if (s_count)
        return;

    // PORT: strikers.ini has to be in the environment before the first getenv here, and main() is
    // too late; this runs *before* main.
    PortConfigLoad();

    const char* env = getenv("STRIKERS_DATA");
    const char* why = "STRIKERS_DATA / the 'data' key in strikers.ini";
    if (env != NULL && *env != '\0')
    {
        snprintf(s_root, sizeof s_root, "%s", env);
        snprintf(tried[nTried], sizeof tried[0], "%s", s_root);
        triedWhy[nTried++] = why;
        if (is_regular_file(s_root))
        {
            // Refused: the player named this path, so no later candidate can make it usable, and the
            // entry point is about to hand the reason to the host.
            if (open_image(s_root) != 0)
                return;
        }
        else
            scan(s_root, "");
    }

    // The host application's own store, for a host that takes the player's disc in itself rather
    // than asking the player to arrange files around an executable (include/port/hostui.h). Ahead of
    // the directory beside the executable because on a sandboxed platform that one is a read-only
    // application bundle. Named in the message only when the host has something to name.
    if (s_count == 0)
    {
        const char* host = PortHostUIGameDataPath();
        if (host != NULL && *host != '\0')
        {
            snprintf(s_root, sizeof s_root, "%s", host);
            snprintf(tried[nTried], sizeof tried[0], "%s", s_root);
            triedWhy[nTried++] = "the application's own store";
            if (is_regular_file(s_root))
            {
                if (open_image(s_root) != 0)
                    return;
            }
            else
                scan(s_root, "");
        }
    }

    // Beside the executable, which is what an unpacked release archive looks like: strikers(.exe)
    // and a files/ directory, nothing to configure.
    if (s_count == 0)
    {
        char dir[1024];
        if (port_executable_dir(dir, sizeof dir) == 0)
        {
            snprintf(s_root, sizeof s_root, "%s/files", dir);
            snprintf(tried[nTried], sizeof tried[0], "%s", s_root);
            triedWhy[nTried++] = "beside the game";
            scan(s_root, "");
        }
    }

    // A disc image beside the game, which is the release archive again with the step of extracting
    // it left out.
    if (s_count == 0)
    {
        char dir[1024];
        struct PickCtx ctx;
        ctx.best[0] = '\0';
        if (port_executable_dir(dir, sizeof dir) == 0)
        {
            ctx.dir = dir;
            port_scan_dir(dir, pick_image, &ctx);
            if (ctx.best[0] != '\0')
            {
                snprintf(s_root, sizeof s_root, "%s/%s", dir, ctx.best);
                snprintf(tried[nTried], sizeof tried[0], "%s", s_root);
                triedWhy[nTried++] = "a disc image beside the game";
                if (open_image(s_root) != 0)
                    return;
            }
        }
    }

    // Last: data/<disc id>/files under the port, so running the binary straight out of a build
    // directory keeps working.
    if (s_count == 0)
    {
        // Any of the three discs will run; this is only which one a developer working from a
        // checkout gets by default.
        snprintf(s_root, sizeof s_root, "%s",
                 "data/G4QE01/files");
        snprintf(tried[nTried], sizeof tried[0], "%s", s_root);
        triedWhy[nTried++] = "a source checkout";
        scan(s_root, "");
    }

    if (s_disc != NULL)
        fprintf(stderr, "[port] DVD: %d files in %s (%s image)\n", s_count,
                s_root, port_disc_format(s_disc));
    else
        fprintf(stderr, "[port] DVD: %d files under %s\n", s_count, s_root);

    // PORT: a refusal rather than zero files and a printed warning; launched from a file manager
    // there is no console to read, and on a host with its own importer this text is what that
    // importer shows instead of the process ending here.
    if (s_count == 0)
    {
        char msg[4096];
        int n = 0;
        int i;
        n += snprintf(msg + n, sizeof msg - (size_t)n,
                      "The game data was not found.\n\n"
                      "Super Mario Strikers needs its own GameCube disc: "
                      "either a disc image\n(.iso, .gcm, .ciso or .gcz) or the "
                      "`files` folder of an extracted one\n(1416 files, about "
                      "618 MB). No game data is shipped with this port; "
                      "supply\nyour own copy.\n\n"
                      "Looked in, in order:\n");
        for (i = 0; i < nTried && n < (int)sizeof msg; i++)
            n += snprintf(msg + n, sizeof msg - (size_t)n,
                          "  %d. %s\n     (%s)\n", i + 1, tried[i], triedWhy[i]);
        if (n < (int)sizeof msg)
            snprintf(msg + n, sizeof msg - (size_t)n,
                     "\nTo fix this, either:\n"
                     "  * choose it on the Game tab of the settings app beside "
                     "the game, or\n"
                     "  * put your disc image, or the extracted disc's `files` "
                     "folder, next to the\n    game, or\n"
                     "  * set the `data` key in strikers.ini (beside the game) "
                     "to either of those, or\n"
                     "  * set the STRIKERS_DATA environment variable to it.\n");
        dvd_refuse("Super Mario Strikers: game data not found", msg);
        return;
    }

    // Files, but not this game's files. A folder of holiday photos scans perfectly well and then
    // dies a long way from here, in the DVD layer or an allocator, on a header that was never a
    // header.
    if (DVDConvertPathToEntrynum(DVD_SENTINEL) < 0)
    {
        char msg[4096];
        snprintf(msg, sizeof msg,
                 "That %s is not the Super Mario Strikers disc.\n\n"
                 "Found %d files in:\n  %s\n\n"
                 "...but no `%s`, which is on every copy of this game.%s\n",
                 s_disc != NULL ? "disc image" : "folder",
                 s_count, s_root, DVD_SENTINEL,
                 s_disc != NULL
                     ? " It is a readable disc image\nof some other game."
                     : " The path is probably\npointing one level too high or "
                       "too low: it must be the `files` folder itself,\nthe one "
                       "that contains `common.ini`, `art` and `audio`.");
        dvd_refuse("Super Mario Strikers: wrong game data", msg);
        return;
    }

    load_disk_id();

    // Say which disc this is, always. It is one line, it is the first thing worth knowing about a
    // run that behaves oddly, and every region-specific decision in the game now follows from it
    // (include/port/region.h).
    {
        static const char* const kNames[3] = { "USA", "Europe", "Japan" };
        fprintf(stderr, "[port] DVD: disc %.6s (%s%s)\n",
                (const char*)disk_id_bytes(), kNames[port_region()],
                s_disk_id_guessed ? ", not read from the data" : "");
    }

    // This game's files, but not this game's disc.
    if (memcmp(disk_id_bytes(), "G4Q", 3) != 0)
    {
        char msg[4096];
        char found[7];
        memcpy(found, disk_id_bytes(), 6);
        found[6] = '\0';
        snprintf(msg, sizeof msg,
                 "The disc header %s is not Super Mario "
                 "Strikers.\n\n"
                 "  Game data:  %s\n"
                 "  It says the disc is: %s\n\n"
                 "Every release of this game has a code beginning G4Q "
                 "(G4QE01 USA, G4QP01\nEurope, G4QJ01 Japan) and this port "
                 "runs any of them. A `files` folder from\none game beside "
                 "another game's `sys` is the usual cause.\n",
                 s_disc != NULL ? "in this image" : "beside this data",
                 s_root, found);
        dvd_refuse("Super Mario Strikers: wrong disc", msg);
        return;
    }
}

// Resolve again from the state the first attempt started from. Called by the port's entry point
// after a host has staged the player's own disc, so that main.cpp needs no knowledge of the DVD
// layer it is re-running.
void PortGameDataResolve(void)
{
    DVDInit();
}

// STRIKERS_PROBE_DVD: name every open and every read, with the bytes asked for against the bytes
// produced.
static int probe_dvd(void)
{
    static int s_on = -1;
    if (s_on < 0)
    {
        const char* e = getenv("STRIKERS_PROBE_DVD");
        s_on = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
    }
    return s_on;
}

s32 DVDConvertPathToEntrynum(const char* pathPtr)
{
    if (!pathPtr)
        return -1;
    // The game spells paths with mixed case and a leading '/' in places; the index is lowercased,
    // so compare case-insensitively and skip any root.
    while (*pathPtr == '/')
        pathPtr++;
    s32 found = -1;
    for (int i = 0; i < s_count; i++)
        // strcmpi, not strcasecmp: prelude.h maps it to the host spelling. strcasecmp is POSIX and
        // absent on Windows, where the same function is _stricmp in <string.h>.
        if (strcmpi(s_entries[i].path, pathPtr) == 0)
        {
            found = i;
            break;
        }
    return found;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo)
{
    if (entrynum < 0 || entrynum >= s_count || !fileInfo)
        return FALSE;
    // Under STRIKERS_LOG_AUDIO, name every stream file opened, whichever route resolved it.
    {
        static int s_log = -1;
        if (s_log < 0)
        {
            const char* e = getenv("STRIKERS_LOG_AUDIO");
            s_log = (e != NULL && *e != '\0') ? 1 : 0;
        }
        if (s_log)
        {
            const char* path = s_entries[entrynum].path;
            size_t n = strlen(path);
            if ((n > 5 && strcmpi(path + n - 5, ".idsp") == 0) || (n > 4 && strcmpi(path + n - 4, ".dsp") == 0))
            {
                /* port_monotonic_ns, not clock_gettime: CLOCK_MONOTONIC is POSIX and the UCRT
                   has neither. port/host.h exists for this family. */
                OSReport("[port] dvd: stream open %s @%lums\n", path,
                         (unsigned long)(port_monotonic_ns() / 1000000ull));
            }
        }
    }
    if (probe_dvd())
        OSReport("[port] dvd: open %s (%u bytes)\n", s_entries[entrynum].path,
                 (unsigned)s_entries[entrynum].length);
    memset(fileInfo, 0, sizeof *fileInfo);
    fileInfo->startAddr = (u32)entrynum;   // index, not a disc offset
    fileInfo->length = s_entries[entrynum].length;
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo)
{
    return DVDFastOpen(DVDConvertPathToEntrynum(fileName), fileInfo);
}


static s32 dvd_read(const DvdEntry* e, void* addr, s32 length, s32 offset);

#if defined(__SWITCH__)
// Pending reads share a fixed-size pool across open files.
#define DVD_PENDING_MAX 24
typedef struct DvdPending
{
    const DvdEntry* entry;
    DVDFileInfo* fileInfo;
    void* addr;
    s32 length;
    s32 offset;
    DVDCallback callback;
    int32_t inUse;
} DvdPending;

static DvdPending s_pending[DVD_PENDING_MAX];

// Reads report busy on their first poll, before GameCubeReadAsync advances the file position.
#define DVD_OWED_MAX 32
// Game thread only, so no lock.
static const DVDCommandBlock* s_owedBusy[DVD_OWED_MAX];

static void owe_busy_poll(const DVDCommandBlock* block)
{
    int i;
    for (i = 0; i < DVD_OWED_MAX; i++)
    {
        if (s_owedBusy[i] == block)
            return;
    }
    for (i = 0; i < DVD_OWED_MAX; i++)
    {
        if (s_owedBusy[i] == NULL)
        {
            s_owedBusy[i] = block;
            return;
        }
    }
    OSReport("[port] DVD: more than %d reads owed a busy poll; one completes a poll early\n",
             DVD_OWED_MAX);
}

static int take_busy_poll(const DVDCommandBlock* block)
{
    int i;
    for (i = 0; i < DVD_OWED_MAX; i++)
    {
        if (s_owedBusy[i] == block)
        {
            s_owedBusy[i] = NULL;
            return 1;
        }
    }
    return 0;
}

static s32 pending_read_now(const DvdEntry* e, void* addr, s32 length, s32 offset);
static void pending_run(void* ctx);

// Only the game thread claims slots and only the reader frees them.
static DvdPending* pending_acquire(void)
{
    int i;
    for (i = 0; i < DVD_PENDING_MAX; i++)
    {
        if (!port_load_acquire_i32(&s_pending[i].inUse))
        {
            s_pending[i].inUse = 1;
            return &s_pending[i];
        }
    }
    return NULL;
}
#endif

BOOL DVDClose(DVDFileInfo* fileInfo)
{
#if defined(__SWITCH__)
    int i;
    if (fileInfo == NULL)
        return TRUE;
    // Wait for this file's queued reads before the caller can free the command block and buffer.
    for (i = 0; i < DVD_PENDING_MAX; i++)
    {
        while (s_pending[i].fileInfo == fileInfo && port_load_acquire_i32(&s_pending[i].inUse))
            port_yield();
    }
    take_busy_poll(&fileInfo->cb);
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
#else
    if (fileInfo)
        fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
#endif
}

s32 DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                     DVDCallback callback, s32 prio)
{
    (void)prio;
    if (!fileInfo || fileInfo->startAddr >= (u32)s_count)
        return -1;
    const DvdEntry* e = &s_entries[fileInfo->startAddr];

    // A null destination or nonsensical length is a caller bug, named here because the UCRT's fread
    // rejects them through _invalid_parameter, aborting from inside the CRT with the caller's
    // numbers nowhere.
    if (addr == NULL || length <= 0)
    {
        OSReport("[port] DVDReadAsyncPrio: refusing %s read of %s: "
                 "addr=%p length=%d offset=%d\n",
                 addr == NULL ? "null-destination" : "non-positive-length",
                 e->host != NULL ? e->host : e->path, addr, (int)length,
                 (int)offset);
        fileInfo->cb.transferredSize = 0;
        fileInfo->cb.state = DVD_STATE_END;
        if (callback)
            callback(-1, fileInfo);
        return -1;
    }

#if defined(__SWITCH__)
    // Read synchronously once the entire image is in RAM.
    if (s_disc != NULL && port_disc_in_memory(s_disc))
    {
        const s32 got = pending_read_now(e, addr, length, offset);
        fileInfo->cb.transferredSize = got > 0 ? (u32)got : 0;
        fileInfo->cb.state = DVD_STATE_END;
        owe_busy_poll(&fileInfo->cb);
        if (callback)
            callback(got, fileInfo);
        return got < 0 ? -1 : 0;
    }

    // Queue the read on the reader thread; with every slot busy, wait rather than share the file.
    DvdPending* job;
    while ((job = pending_acquire()) == NULL)
        port_yield();

    job->entry = e;
    job->addr = addr;
    job->length = length;
    job->offset = offset;
    job->callback = callback;
    job->fileInfo = fileInfo;
    fileInfo->cb.transferredSize = 0;
    fileInfo->cb.state = DVD_STATE_BUSY;
    owe_busy_poll(&fileInfo->cb);
    PortDiscQueue(pending_run, job);
    return 0;
#else
    const s32 got = dvd_read(e, addr, length, offset);

    if (probe_dvd())
        OSReport("[port] dvd: read %s off=%d len=%d -> %d cb=%d\n", e->path,
                 (int)offset, (int)length, (int)got, callback != NULL);

    fileInfo->cb.transferredSize = got > 0 ? (u32)got : 0;
    if (callback)
    {
        // A caller that asked for a callback gets it here, inline; nothing in this tree does, and
        // there is no later point from which to fire it.
        fileInfo->cb.state = DVD_STATE_END;
        callback(got, fileInfo);
    }
    else
    {
        // Done, but busy to the first poll. See the header comment.
        fileInfo->cb.state = DVD_STATE_BUSY;
    }
    return got < 0 ? -1 : 0;
#endif
}

static s32 dvd_read(const DvdEntry* e, void* addr, s32 length, s32 offset)
{
    s32 got = -1;
    if (offset < 0)
    {
        got = -1;
    }
    else if (s_disc != NULL)
    {
        // Clamp to the file. A host file gets this free from fread, but on an image the next file
        // sits right there, so an over-long read would return the neighbour's bytes.
        const u32 at = (u32)offset;
        if (at >= e->length)
        {
            got = 0;
        }
        else
        {
            u32 avail = e->length - at;
            size_t want = (size_t)length < (size_t)avail ? (size_t)length
                                                         : (size_t)avail;
            got = (s32)port_disc_read(s_disc, addr, want,
                                      (unsigned long long)e->offset + at);
        }
    }
    else
    {
        FILE* f = fopen(e->host, "rb");
        if (f)
        {
            if (fseek(f, offset, SEEK_SET) == 0)
                got = (s32)fread(addr, 1, (size_t)length, f);
            fclose(f);
        }
    }
    return got;
}

#if defined(__SWITCH__)
static s32 pending_read_now(const DvdEntry* e, void* addr, s32 length, s32 offset)
{
    const s32 got = dvd_read(e, addr, length, offset);
    if (probe_dvd())
        OSReport("[port] dvd: read %s off=%d len=%d -> %d\n", e->path,
                 (int)offset, (int)length, (int)got);
    return got;
}

static void pending_run(void* ctx)
{
    DvdPending* job = (DvdPending*)ctx;
    const s32 got = pending_read_now(job->entry, job->addr, job->length, job->offset);
    DVDFileInfo* fileInfo = job->fileInfo;
    DVDCallback callback = job->callback;

    fileInfo->cb.transferredSize = got > 0 ? (u32)got : 0;
    // A poll that sees the new state also sees the size and the bytes.
    port_store_release_i32(&fileInfo->cb.state, DVD_STATE_END);
    if (callback)
        callback(got, fileInfo);
    // Released last: DVDClose may be waiting on it to free the file.
    port_store_release_i32(&job->inUse, 0);
}
#endif

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block)
{
    if (!block)
        return DVD_STATE_END;
#if defined(__SWITCH__)
    if (take_busy_poll(block))
        return DVD_STATE_BUSY;
    return port_load_acquire_i32(&block->state);
#else
    if (block->state == DVD_STATE_BUSY)
    {
        // The one poll that says busy. The console's own status call read a block the drive was
        // still writing to through the same const pointer; the cast is the same lie it told.
        ((DVDCommandBlock*)block)->state = DVD_STATE_END;
        return DVD_STATE_BUSY;
    }
    return block->state;
#endif
}

s32 DVDGetDriveStatus(void)
{
    return 0;   // DVD_STATE_END: drive idle, no error, disc present
}

static int ieq(const char* a, const char* b) { return strcmpi(a, b) == 0; }

// `region` in strikers.ini: the disc the files came from, for an extraction with no sys/boot.bin.
static const char* region_id_from_setting(const char* v)
{
    static const char* const kIds[3] = { "G4QE01", "G4QP01", "G4QJ01" };
    static const char* const kNames[3][4] = {
        { "usa", "us", "ntsc-u", "0" },
        { "europe", "eur", "pal", "1" },
        { "japan", "jap", "ntsc-j", "2" },
    };
    int r, k;

    if (v == NULL || *v == '\0')
        return NULL;
    for (r = 0; r < 3; r++)
    {
        if (ieq(v, kIds[r]))
            return kIds[r];
        for (k = 0; k < 4; k++)
            if (ieq(v, kNames[r][k]))
                return kIds[r];
    }
    return NULL;
}

// PORT: split out of DVDGetCurrentDiskID so the id is read at DVDInit, before anything asks.
// port_region() answers every region conditional from these six bytes, and DVDInit runs before the
// first static initialiser.
static void load_disk_id(void)
{
    char p[1200];
    FILE* f;

    if (s_disk_id_loaded)
        return;
    s_disk_id_loaded = 1;

    // An image has already filled these in from its own first 32 bytes, which is exactly what
    // sys/boot.bin is a copy of.
    if (!s_disk_id_read)
    {
        snprintf(p, sizeof p, "%s/../sys/boot.bin", s_root);
        f = fopen(p, "rb");
        if (f)
        {
            s_disk_id_read = fread(s_disk_id, 1, sizeof s_disk_id, f)
                             == sizeof s_disk_id;
            fclose(f);
        }
    }
    // No sys/ beside files/: the player's `region` names the disc, and failing that USA, said out
    // loud because a European or Japanese extraction would otherwise run as the wrong game.
    if (!s_disk_id_read)
    {
        const char* setting = getenv("STRIKERS_REGION");
        const char* id = region_id_from_setting(setting);

        s_disk_id_guessed = 1;
        if (id != NULL)
        {
            memcpy(s_disk_id, id, 6);
            fprintf(stderr, "[port] DVD: no %s; disc id %s from the region "
                            "setting\n", p, id);
        }
        else
        {
            if (setting != NULL && *setting != '\0')
                fprintf(stderr, "[port] DVD: region=%s is not a region "
                                "(usa, europe, japan, or a disc id G4QE01 / "
                                "G4QP01 / G4QJ01); ignored\n", setting);
            memcpy(s_disk_id, "G4QE01", 6);
            fprintf(stderr, "[port] DVD: WARNING: no %s, so the disc id is a "
                            "guess: USA (G4QE01). A European or Japanese "
                            "extraction needs the disc's sys/ folder beside "
                            "files/ (tools/extract-disc.py writes it) or "
                            "`region` in strikers.ini.\n", p);
        }
    }
    else if (getenv("STRIKERS_REGION") != NULL && *getenv("STRIKERS_REGION") != '\0')
    {
        fprintf(stderr, "[port] DVD: region setting ignored: the data carries "
                        "its own disc id\n");
    }

    // Aurora derives the memory card's region directory from this; see src/platform/region.cpp.
    PortSetDiscGameName((const char*)s_disk_id);
}

void* DVDGetCurrentDiskID(void)
{
    load_disk_id();
    return s_disk_id;
}

static const u8* disk_id_bytes(void)
{
    load_disk_id();
    return s_disk_id;
}

// The whole of the runtime region decision, and the reason this port needs no per-region build.
int port_region(void)
{
    if (s_region_cache < 0)
    {
        const u8* id = disk_id_bytes();
        if (id[0] == 'G' && id[1] == '4' && id[2] == 'Q' && id[3] == 'P')
            s_region_cache = PORT_REGION_EUROPE;
        else if (id[0] == 'G' && id[1] == '4' && id[2] == 'Q' && id[3] == 'J')
            s_region_cache = PORT_REGION_JAPAN;
        else
            s_region_cache = PORT_REGION_USA;
    }
    return s_region_cache;
}

const char* port_disc_game_code(void)
{
    if (s_game_code[0] == 0)
    {
        memcpy(s_game_code, disk_id_bytes(), 4);
        s_game_code[4] = '\0';
    }
    return s_game_code;
}

const char* port_disc_maker_code(void)
{
    if (s_maker_code[0] == 0)
    {
        memcpy(s_maker_code, disk_id_bytes() + 4, 2);
        s_maker_code[2] = '\0';
    }
    return s_maker_code;
}
