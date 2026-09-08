#pragma once
#include <glib.h>

// One timetable entry from animeschedule.net (this week, sub releases).
typedef struct {
    char *title;   // display title
    char *romaji;
    char *english;
    GDateTime *date; // episodeDate (UTC)
    int episode;     // episodeNumber
    char *airing_status; // "aired" / "airing" / "unaired" / "delayed-air"
} ScheduleEntry;

// Fired on the main loop. entries is owned by the module and valid until
// the next fetch completes; NULL when no token is set or the fetch failed.
typedef void (*ScheduleCallback)(GPtrArray *entries, gpointer user_data);

void schedule_init(void);
const char *schedule_token(void); // NULL/empty when unset
void schedule_set_token(const char *token); // persists, clears cache
// Cached ~30 min; multiple callers during one fetch are coalesced.
void schedule_fetch(ScheduleCallback cb, gpointer user_data);
