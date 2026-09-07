#pragma once
#include <glib.h>

// One AniList media entry, as much as we need for completeness tracking.
typedef struct {
    gboolean ok;    // found on AniList
    int episodes;   // total planned episodes, -1 if unknown
    int aired;      // episodes aired so far (airing shows), -1 if n/a
    gboolean airing;
    char *title;    // matched romaji title
    char *season;   // "WINTER"/"SPRING"/"SUMMER"/"FALL", NULL if unknown
    int season_year; // e.g. 2023, -1 if unknown
    char *source;   // "AniList" or "Kitsu" (fallback provider)
} AniInfo;

// Called on the main loop. info is owned by the cache — copy what you keep.
typedef void (*AniCallback)(const AniInfo *info, gpointer user_data);

void anilist_init(void);
// Cached + rate-limited async lookup; cb always fires exactly once (with
// info->ok=FALSE when the API is down or nothing matched).
void anilist_lookup(const char *search, AniCallback cb, gpointer user_data);
// Same, but bypasses the cache and re-fetches (still rate-limited).
void anilist_refresh(const char *search, AniCallback cb, gpointer user_data);
