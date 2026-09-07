#pragma once
#include <adwaita.h>
#include <gtk/gtk.h>

// Shared library-scan helpers (implemented in main.c).
void regexes_init(void);
int episode_number(const char *fname);
gboolean is_video(const char *name);
// Numbers absent between min and max of present. Caller unrefs.
GArray *find_gaps(GArray *present);

// ---------------------------------------------------------------- providers --

typedef enum {
    FETCH_SUBSPLEASE = 0,
    FETCH_ERAI = 1,
} FetchProvider;

typedef struct {
    int episode;     // wanted episode number
    char *title;     // release title, e.g. "[Erai-raws] Frieren - 07 ..."
    char *magnet;    // magnet URI (may be NULL if only torrent_url)
    char *torrent_url; // direct .torrent URL (may be NULL)
    char *size;      // human-readable size, may be NULL
    int seeders;     // -1 when unknown (SubsPlease API has no counts)
} FetchResult;

void fetch_result_free(gpointer p);

// Async search for one episode. cb fires exactly once on the main loop with
// a (possibly empty, never NULL) GPtrArray of FetchResult* (best first).
// The array is owned by the callee — copy what you keep.
typedef void (*FetchCallback)(GPtrArray *results, gpointer user_data);
void fetch_search(FetchProvider provider, const char *show, int episode,
                  const char *quality, FetchCallback cb, gpointer user_data);

// Save the .torrent (when known) into dest_dir, then hand the magnet/torrent
// to a downloader: aria2c straight into dest_dir when available, otherwise
// the desktop's torrent client via xdg-open. cb fires once on completion
// (for xdg-open: once the client was launched).
typedef void (*FetchDlCallback)(gboolean ok, const char *message,
                                gpointer user_data);
void fetch_download(const FetchResult *res, const char *dest_dir,
                    FetchDlCallback cb, gpointer user_data);

gboolean fetch_have_aria2(void);

// ------------------------------------------------------------------ dialog --

// One episode-bearing directory on the series page.
typedef struct {
    char *search; // lookup term for this group ("Series" / "Series Season 2")
    char *label;  // "Episodes" / "Season 2"
    char *dir;    // filesystem path to download into
} FetchGroup;

FetchGroup *fetch_group_new(const char *search, const char *label,
                            const char *dir);
void fetch_group_free(gpointer p);

// Manual fetch dialog for a series. Gaps are listed immediately; AniList is
// consulted for episodes aired past the local max. done is invoked on close
// (e.g. to refresh the library) and may be NULL. Returns the dialog.
AdwDialog *fetch_dialog_show(GtkWindow *parent, const char *series,
                             GPtrArray *groups /* FetchGroup* */,
                             void (*done)(gpointer), gpointer done_data);
