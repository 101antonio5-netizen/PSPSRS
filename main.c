#include <stddef.h>
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <intraFont.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

PSP_MODULE_INFO("SRS Decks", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);

#define MAX_CARDS         2000
#define MAX_DECKS         64
#define MENU_ROWS         8     /* deck rows visible at once */
#define FRONT_LEN         128   /* bytes (UTF-8: an accented letter takes 2) */
#define BACK_LEN          256
#define ID_LEN            32
#define LINE_LEN          1024
#define SAVE_EVERY        10    /* save after this many changes (reviews / new cards) */
#define DAY_ROLLOVER_HOUR 4     /* the "study day" changes at 04:00 local time, like Anki */
#define SRS_VERSION       1

#define DEFAULT_NEW_PER_DAY      10    /* never-seen cards introduced per day (0 = none) */
#define DEFAULT_REVIEWS_PER_DAY  200   /* answers on already-learned cards per day (0 = no limit) */
#define MAX_NEW_LIMIT            200
#define MAX_REVIEW_LIMIT         999

/* Folders scanned for .tsv decks. ms0: is the memory stick (and PPSSPP's
   memstick folder); ef0: is the internal storage of a PSP Go. */
static const char *deck_dirs[] = { "ms0:/decks", "ef0:/decks" };
#define NUM_DECK_DIRS 2

/* ---------- HOME button support ----------
   The callback only sets a flag: the main loop saves progress and then exits. */

static volatile int quit_requested = 0;

static int exit_callback(int arg1, int arg2, void *common) {
    quit_requested = 1;
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread, 0x11, 0xFA0, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
}

/* ---------- Screen, colors, fonts ---------- */

#define BUF_WIDTH    512     /* framebuffer row length in pixels (must be 512) */
#define SCR_W        480
#define SCR_H        272
#define HEADER_H     30
#define FOOTER_Y     250
#define LINE_H_BASE  17.0f   /* line height of the font at size 1.0, in pixels */

/* Colors are 0xAABBGGRR (the GU's byte order), so build them with RGB()/RGBA(). */
#define RGBA(r, g, b, a) (((unsigned)(a) << 24) | ((unsigned)(b) << 16) | ((unsigned)(g) << 8) | (unsigned)(r))
#define RGB(r, g, b)     RGBA(r, g, b, 255)

#define COL_BG      RGB(20, 24, 33)
#define COL_PANEL   RGB(30, 36, 50)
#define COL_CARD    RGB(40, 48, 68)
#define COL_ACCENT  RGB(88, 150, 255)
#define COL_TEXT    RGB(245, 247, 250)
#define COL_DIM     RGB(150, 160, 182)
#define COL_OK      RGB(110, 200, 130)
#define COL_WARN    RGB(214, 150, 52)
#define COL_ERR     RGB(200, 60, 60)
#define COL_AGAIN   RGB(190, 70, 70)
#define COL_HARD    RGB(200, 130, 50)
#define COL_GOOD    RGB(60, 150, 90)
#define COL_EASY    RGB(60, 120, 205)

/* This SDK's intraFontSetStyle takes a rotation angle (in degrees) between the
   shadow color and the options. We never rotate text, so it is always 0. */
#define SET_STYLE(f, size, col, shadow, opts) \
    intraFontSetStyle((f), (size), (col), (shadow), 0.0f, (opts))

/* The display list: the GPU commands for one frame are collected here. */
static unsigned int __attribute__((aligned(16))) gu_list[262144];

static void gu_setup(void) {
    /* VRAM offsets of the two framebuffers (draw + display, swapped each frame). */
    void *fbp0 = (void *)0;
    void *fbp1 = (void *)(BUF_WIDTH * SCR_H * 4);

    sceGuInit();
    sceGuStart(GU_DIRECT, gu_list);
    sceGuDrawBuffer(GU_PSM_8888, fbp0, BUF_WIDTH);
    sceGuDispBuffer(SCR_W, SCR_H, fbp1, BUF_WIDTH);
    sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);

    /* Alpha blending: needed for smooth text edges. */
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void begin_frame(void) {
    sceGuStart(GU_DIRECT, gu_list);
    sceGuClearColor(COL_BG);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

static void end_frame(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();   /* also paces the loop at 60 frames per second */
    sceGuSwapBuffers();
}

typedef struct {
    unsigned int color;
    float x, y, z;
} Vertex;

#define VERTEX_FORMAT (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

/* Filled rectangle in screen pixels, (0,0) = top-left corner. */
static void fill_rect(float x, float y, float w, float h, unsigned int color) {
    Vertex *v = (Vertex *)sceGuGetMemory(2 * sizeof(Vertex));

    v[0].color = color; v[0].x = x;     v[0].y = y;     v[0].z = 0.0f;
    v[1].color = color; v[1].x = x + w; v[1].y = y + h; v[1].z = 0.0f;

    sceGuDisable(GU_TEXTURE_2D);        /* intraFont leaves texturing on */
    sceGuDrawArray(GU_SPRITES, VERTEX_FORMAT, 2, NULL, v);
}

static intraFont *font = NULL;

static float text_width(const char *s, float size) {
    if (!font) return (float)strlen(s) * 7.0f * size;
    SET_STYLE(font, size, COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
    return intraFontMeasureText(font, s);
}

/* One line of text; (x, y) is the left/center/right end of the baseline, depending on
   options (INTRAFONT_ALIGN_LEFT / _CENTER / _RIGHT). */
static void draw_text(float x, float y, float size, unsigned int color,
                      unsigned int options, const char *s) {
    if (!font) return;
    SET_STYLE(font, size, color, 0, options);
    intraFontPrint(font, x, y, s);
}

/* Shorten 's' with "..." until it fits in 'width' pixels (never cuts a UTF-8 character). */
static void fit_text(const char *s, float width, float size, char *out, size_t outsz) {
    size_t n;

    snprintf(out, outsz, "%s", s);
    if (text_width(out, size) <= width) return;

    n = strlen(out);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;   /* back to the start of a character */
        out[n] = '\0';
        if (n + 4 <= outsz) {
            strcat(out, "...");
            if (text_width(out, size) <= width) return;
            out[n] = '\0';
        }
    }
}

#define MAX_WRAP_LINES  10
#define WRAP_LINE_LEN   300
static char wrap_buf[MAX_WRAP_LINES][WRAP_LINE_LEN];

/* Greedy word wrap of 's' into wrap_buf. Returns the number of lines. */
static int wrap_text(const char *s, float width, float size) {
    int nlines = 0;
    char cur[WRAP_LINE_LEN], trial[WRAP_LINE_LEN], word[WRAP_LINE_LEN];
    const char *p = s;

    cur[0] = '\0';
    while (*p) {
        size_t wl = 0;

        while (*p == ' ') p++;
        if (!*p) break;
        while (*p && *p != ' ' && wl < sizeof(word) - 1) word[wl++] = *p++;
        word[wl] = '\0';

        if (cur[0] == '\0') snprintf(trial, sizeof(trial), "%s", word);
        else                snprintf(trial, sizeof(trial), "%s %s", cur, word);

        if (cur[0] != '\0' && text_width(trial, size) > width) {
            if (nlines < MAX_WRAP_LINES) snprintf(wrap_buf[nlines++], WRAP_LINE_LEN, "%s", cur);
            snprintf(cur, sizeof(cur), "%s", word);
        } else {
            snprintf(cur, sizeof(cur), "%s", trial);
        }
    }
    if (cur[0] != '\0' && nlines < MAX_WRAP_LINES)
        snprintf(wrap_buf[nlines++], WRAP_LINE_LEN, "%s", cur);
    return nlines;
}

/* Wrapped, centered text block, vertically centered on cy. If it is taller than max_h
   pixels the text is shrunk until it fits (down to a minimum size). */
static void draw_block(const char *s, float cx, float cy, float width, float size,
                       float max_h, unsigned int color) {
    int n, i;
    float lh, y;

    for (;;) {
        n = wrap_text(s, width, size);
        if ((float)n * size * LINE_H_BASE <= max_h || size <= 0.65f) break;
        size *= 0.88f;
    }
    lh = size * LINE_H_BASE;
    y  = cy - ((float)(n - 1) * lh) / 2.0f + size * 5.0f;   /* baseline of the first line */
    for (i = 0; i < n; i++)
        draw_text(cx, y + (float)i * lh, size, color, INTRAFONT_ALIGN_CENTER, wrap_buf[i]);
}

/* Wrapped, left-aligned paragraph; y is the baseline of the first line. */
static void draw_para(const char *s, float x, float y, float width, float size,
                      int max_lines, unsigned int color) {
    int n = wrap_text(s, width, size), i;
    if (n > max_lines) n = max_lines;
    for (i = 0; i < n; i++)
        draw_text(x, y + (float)i * size * LINE_H_BASE, size, color, INTRAFONT_ALIGN_LEFT, wrap_buf[i]);
}

/* Key repeat: fires on the first press, then every 3 frames after the key was held ~1/3 s. */
static int key_repeat(int *hold, int down, int edge) {
    if (!down) { *hold = 0; return 0; }
    (*hold)++;
    return edge || (*hold > 20 && (*hold % 3) == 0);
}

/* ---------- Cards ---------- */

typedef struct {
    char  front[FRONT_LEN];
    char  back[BACK_LEN];
    char  id[ID_LEN];   /* optional third column of the deck (Anki GUID), may be empty */
    int   reps;         /* consecutive successful reviews */
    int   interval;     /* days */
    float ease;         /* ease factor, starts at 2.5 */
    long  due;          /* absolute day number on which the card is due */
    int   introduced;   /* 0 = never shown yet */
    int   accented;     /* 1 = the front text contains non-ASCII characters */
} Card;

static Card cards[MAX_CARDS];
static int  num_cards = 0;
static int  new_today = 0;   /* new cards introduced during the current study day */
static int  unsaved   = 0;   /* changes since the last successful save */

/* Daily limits (saved per deck in the .srs file) and today's counters */
static int  new_limit     = DEFAULT_NEW_PER_DAY;
static int  review_limit  = DEFAULT_REVIEWS_PER_DAY;
static int  reviews_today = 0;   /* answers given today to cards that were already learned */

/* Loader statistics for the deck currently loaded */
static int skipped_lines  = 0;
static int truncated      = 0;
static int replaced_chars = 0;

/* Progress (.srs) state for the deck currently loaded */
static int  ids_ok            = 0;   /* 1 = every card has a unique ID, use IDs as keys */
static int  progress_found    = 0;
static int  progress_restored = 0;
static int  progress_bad      = 0;
static long last_study_day    = 0;
static int  saved_new_today   = 0;
static int  saved_reviews_today = 0;
static int  clock_clamped     = 0;

/* Study-day bookkeeping */
static long base_day   = 0;   /* real study day (never goes backwards during a session) */
static long day_offset = 0;   /* debug: days skipped with the R trigger (disables saving) */

/* ---------- Deck list ---------- */

typedef struct {
    char name[104];    /* file name without the .tsv extension */
    char path[192];    /* full path, e.g. ms0:/decks/french.tsv */
} DeckEntry;

static DeckEntry decks[MAX_DECKS];
static int  num_decks    = 0;
static int  current_deck = -1;
static char menu_msg[128]   = "";
static char status_msg[128] = "";

static int has_tsv_ext(const char *s) {
    size_t n = strlen(s);
    const char *e;
    if (n < 5) return 0;                 /* at least "x.tsv" */
    e = s + n - 4;
    return e[0] == '.' && tolower((unsigned char)e[1]) == 't' &&
           tolower((unsigned char)e[2]) == 's' && tolower((unsigned char)e[3]) == 'v';
}

static void scan_dir(const char *dir) {
    SceUID d = sceIoDopen(dir);
    SceIoDirent ent;

    if (d < 0) return;

    for (;;) {
        size_t len;
        DeckEntry *e;

        memset(&ent, 0, sizeof(ent));          /* required before sceIoDread */
        if (sceIoDread(d, &ent) <= 0) break;
        if (num_decks >= MAX_DECKS) break;

        if (FIO_S_ISDIR(ent.d_stat.st_mode)) continue;   /* skip subfolders */
        if (ent.d_name[0] == '.') continue;              /* skip hidden / "._x.tsv" junk */
        if (!has_tsv_ext(ent.d_name)) continue;

        len = strlen(ent.d_name);
        if (len >= sizeof(e->name) + 4) continue;        /* name too long for the list */

        e = &decks[num_decks++];
        memcpy(e->name, ent.d_name, len - 4);            /* drop ".tsv" */
        e->name[len - 4] = '\0';
        snprintf(e->path, sizeof(e->path), "%s/%s", dir, ent.d_name);
    }

    sceIoDclose(d);
}

static void scan_decks(void) {
    int i, j;
    num_decks = 0;

    for (i = 0; i < NUM_DECK_DIRS; i++)
        scan_dir(deck_dirs[i]);

    /* Insertion sort by name so the menu order is stable. */
    for (i = 1; i < num_decks; i++) {
        DeckEntry tmp = decks[i];
        j = i - 1;
        while (j >= 0 && strcmp(decks[j].name, tmp.name) > 0) {
            decks[j + 1] = decks[j];
            j--;
        }
        decks[j + 1] = tmp;
    }
}

/* ---------- Dates ---------- */

/* Days since 1970-01-01 for a calendar date (proleptic Gregorian). */
static long days_from_civil(int y, int m, int d) {
    long era;
    unsigned yoe, doy, doe;
    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static void civil_from_days(long z, int *y, int *m, int *d) {
    long era, yy;
    unsigned doe, yoe, doy, mp, dd, mm;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy  = (long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    dd  = doy - (153 * mp + 2) / 5 + 1;
    mm  = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *d = (int)dd;
}

/* Current study day from the PSP's local clock. */
static long get_real_day(void) {
    ScePspDateTime t;
    long day;
    if (sceRtcGetCurrentClockLocalTime(&t) < 0) return base_day;   /* clock unavailable */
    day = days_from_civil(t.year, t.month, t.day);
    if (t.hour < DAY_ROLLOVER_HOUR) day--;
    return day;
}

static long cur_today(void) {
    return base_day + day_offset;
}

/* ---------- Loading a deck ---------- */

static int has_non_ascii(const char *s) {
    for (; *s; s++)
        if ((unsigned char)*s >= 0x80) return 1;
    return 0;
}

/* Copy one field into a fixed buffer, keeping valid UTF-8 text intact. Control
   characters become spaces and invalid UTF-8 bytes become '?'. A field that is too
   long is cut at a character boundary (never in the middle of an accented letter). */
typedef struct {
    int truncated;
    int replaced_bytes;
} FieldStats;

static FieldStats copy_fields(char *dst, size_t dst_size, const char *src, size_t src_len) {
    FieldStats stats = { 0, 0 };
    size_t i = 0, o = 0;
    int cut = 0;

    if (dst_size == 0) return stats;
    dst[0] = '\0';

    while (i < src_len) {
        unsigned char ch = (unsigned char)src[i];
        size_t n = 1;
        int valid = 1;

        if (ch < 0x20) {
            ch = ' ';
        } else if (ch < 0x80) {
            /* ASCII */
        } else if (ch >= 0xC2 && ch <= 0xDF) {
            n = 2;
        } else if (ch >= 0xE0 && ch <= 0xEF) {
            n = 3;
        } else if (ch >= 0xF0 && ch <= 0xF4) {
            n = 4;
        } else {
            valid = 0;
        }

        if (valid && n > 1) {
            size_t k;

            if (i + n > src_len) {
                valid = 0;
            } else {
                if (n == 2) {
                    if (ch < 0xC2) valid = 0;
                } else if (n == 3) {
                    unsigned char c1 = (unsigned char)src[i + 1];
                    if (ch == 0xE0 && c1 < 0xA0) valid = 0;

                   } else if (n == 4) {
                    unsigned char c1 = (unsigned char)src[i + 1];
                    if (ch == 0xF0 && c1 < 0x90) valid = 0;
                    if (ch == 0xF4 && c1 > 0x8F) valid = 0;
                }

                for (k = 1; valid && k < n; k++) {
                    if (((unsigned char)src[i + k] & 0xC0) != 0x80)
                        valid = 0;
                }
            }
        }

        if (!valid) {
            /* One '?' per invalid byte, but never write past dst_size. */
            if (o + 2 > dst_size) {
                cut = 1;
                break;
            }
            dst[o++] = '?';
            dst[o] = '\0';
            stats.replaced_bytes++;
            i++;
            continue;
        }

        if (o + n + 1 > dst_size) {
            cut = 1;
            break;
        }

        memcpy(dst + o, src + i, n);
        o += n;
        dst[o] = '\0';
        i += n;
    }

    if (cut) stats.truncated;
    return stats;
}
/* Returns the number of cards loaded, or -1 if the file can't be opened. */
static int load_deck(const char *path) {
    static char line[LINE_LEN];
    FILE *f = fopen(path, "r");
    int first = 1;

    num_cards      = 0;
    new_today      = 0;
    reviews_today  = 0;
    unsaved        = 0;
    skipped_lines  = 0;
    truncated      = 0;
    replaced_chars = 0;

    if (!f) return -1;

    while (num_cards < MAX_CARDS && fgets(line, sizeof(line), f)) {
        char *start = line;
        char *tab1, *tab2, *tab3, *back;
        size_t len = strlen(line);
        size_t front_len, back_len;
        Card *c;
        FieldStats field_stats;

        /* Line longer than the buffer: throw away the rest of it. */
        if (len > 0 && line[len - 1] != '\n' && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') { }
        }

        /* Strip the line ending (\n or \r\n). */
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        /* Skip a UTF-8 byte-order mark on the first line, if present. */
        if (first) {
            first = 0;
            if (len >= 3 && (unsigned char)start[0] == 0xEF &&
                (unsigned char)start[1] == 0xBB && (unsigned char)start[2] == 0xBF) {
                start += 3;
                len -= 3;
            }
        }

        if (len == 0) continue;               /* blank line: ignore */

        tab1 = strchr(start, '\t');
        if (!tab1) { skipped_lines++; continue; }

        back = tab1 + 1;
        tab2 = strchr(back, '\t');            /* optional third column: card ID */
        front_len = (size_t)(tab1 - start);
        back_len  = tab2 ? (size_t)(tab2 - back) : strlen(back);

        if (front_len == 0 || back_len == 0) { skipped_lines++; continue; }

        c = &cards[num_cards++];
        /*copy_field(c->front, FRONT_LEN, start, front_len);
        copy_field(c->back,  BACK_LEN,  back,  back_len);
        */
        field_stats = copy_field(c->front, FRONT_LEN, start, front_len);
        truncated += field_stats.truncated;
        replaced_chars += field_stats.replaced_bytes;

        field_stats = copy_field(c->back, BACK_LEN, back, back_len);
        truncated += field_stats.truncated;
        replaced_chars += field_stats.replaced_bytes;
       
        c->id[0] = '\0';
        if (tab2) {
            /*int t_save = truncated, r_save = replaced_chars;
            const char *idp = tab2 + 1;
            tab3 = strchr(idp, '\t');
            copy_field(c->id, ID_LEN, idp, tab3 ? (size_t)(tab3 - idp) : strlen(idp));
            truncated = t_save;               /* IDs don't count in the loader stats */
            replaced_chars = r_save; 
            */
            (void)copy_field(c->id, ID_LEN, idp,
                             tab3 ? (size_t)(tab3 - idp) : strlen(idp));
        }

        c->reps       = 0;
        c->interval   = 0;
        c->ease       = 2.5f;
        c->due        = 0;
        c->introduced = 0;
        c->accented   = has_non_ascii(c->front);
    }

    fclose(f);
    return num_cards;
}

/* ---------- Progress file (.srs) ----------

   Format (tab separated, one card per line):
     #srs <version> <keymode g|f> <last study day> <new cards today> <reviews today> <new limit> <review limit>
   (the last three header fields are optional when reading, so files from the
   previous build still load and simply get the default limits)
     <key> <reps> <interval> <ease> <due day>
   keymode g = key is the card ID (third deck column), f = key is the front text. */

/* Use IDs as keys only if EVERY card has one and they are all different. */
static void decide_key_mode(void) {
    int i, j;
    ids_ok = (num_cards > 0);
    for (i = 0; i < num_cards && ids_ok; i++)
        if (!cards[i].id[0]) ids_ok = 0;
    for (i = 0; i < num_cards && ids_ok; i++)
        for (j = i + 1; j < num_cards; j++)
            if (strcmp(cards[i].id, cards[j].id) == 0) { ids_ok = 0; break; }
}

static const char *key_of(const Card *c)            { return ids_ok ? c->id : c->front; }
static const char *key_field(const Card *c, char m) { return m == 'g' ? c->id : c->front; }

static void srs_path_for(const char *deck_path, char *out, size_t size) {
    size_t n = strlen(deck_path);                    /* deck paths always end in ".tsv" */
    snprintf(out, size, "%.*s.srs", (int)(n - 4), deck_path);
}

/* Reduce accented Latin text to plain ASCII. Only used to match progress saved by
   builds that stored accent-stripped card fronts (decks converted with --ascii on). */
static void fold_ascii(const char *s, char *out, size_t size) {
    static const char *const latin1[64] = {   /* U+00C0 .. U+00FF */
        "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
        "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","TH","ss",
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y"
    };
    const unsigned char *p = (const unsigned char *)s;
    size_t o = 0;

    while (*p && o + 4 < size) {
        const char *r;
        char one[2] = { 0, 0 };

        if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) { r = latin1[p[1] - 0x80]; p += 2; }
        else if (p[0] == 0xC5 && p[1] == 0x93)            { r = "oe"; p += 2; }
        else if (p[0] == 0xC5 && p[1] == 0x92)            { r = "OE"; p += 2; }
        else if (p[0] == 0xC2 && p[1] == 0xA0)            { r = " ";  p += 2; }
        else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) { r = "'";   p += 3; }
        else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x9C || p[2] == 0x9D)) { r = "\"";  p += 3; }
        else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x93 || p[2] == 0x94)) { r = "-";   p += 3; }
        else if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0xA6)                   { r = "..."; p += 3; }
        else if (p[0] >= 0x80) {                       /* any other non-ASCII character */
            r = "?";
            p++;
            while ((*p & 0xC0) == 0x80) p++;
        } else { one[0] = (char)p[0]; r = one; p++; }

        while (*r && o + 1 < size) out[o++] = *r++;
    }
    out[o] = '\0';
}

static int fold_hits = 0, fold_misses = 0;

/* First not-yet-restored card with this key; 'hint' is where the previous one matched. */
static int find_unrestored(const char *key, char km, int hint) {
    int i;
    if (hint < num_cards && !cards[hint].introduced &&
        strcmp(key_field(&cards[hint], km), key) == 0)
        return hint;
    for (i = 0; i < num_cards; i++)
        if (!cards[i].introduced && strcmp(key_field(&cards[i], km), key) == 0)
            return i;

    /* Fallback for decks that now keep their accents: match against the accent-stripped
       front. Only cards that actually have accents can match this way, and it gives up
       after 40 misses in a row so a wrong deck can't make loading slow. */
    if (km == 'f' && !(fold_misses >= 40 && fold_hits == 0)) {
        char fk[FRONT_LEN * 2], ff[FRONT_LEN * 2];
        fold_ascii(key, fk, sizeof(fk));
        for (i = 0; i < num_cards; i++) {
            if (cards[i].introduced || !cards[i].accented) continue;
            fold_ascii(cards[i].front, ff, sizeof(ff));
            if (strcmp(ff, fk) == 0) { fold_hits++; return i; }
        }
        fold_misses++;
    }
    return -1;
}

/* Returns 0 on success (including "no progress file yet"), -1 if the deck must not be opened. */
static int load_progress(const char *deck_path) {
    static char line[LINE_LEN];
    char p[200], t[208], km = 'f';
    FILE *f;
    int ver = 0, nt = 0, rt = 0, nl = 0, rl = 0, nparsed = 0, hint = 0, saved_lines = 0;
    long ld = 0;

    progress_found = progress_restored = progress_bad = 0;
    fold_hits = fold_misses = 0;
    last_study_day = 0;
    saved_new_today = 0;
    saved_reviews_today = 0;
    new_limit = DEFAULT_NEW_PER_DAY;
    review_limit = DEFAULT_REVIEWS_PER_DAY;

    srs_path_for(deck_path, p, sizeof(p));
    snprintf(t, sizeof(t), "%s.tmp", p);

    f = fopen(p, "r");
    if (!f) f = fopen(t, "r");     /* a save was interrupted between delete and rename */
    if (!f) return 0;
    progress_found = 1;

    if (!fgets(line, sizeof(line), f) ||
        (nparsed = sscanf(line, "#srs\t%d\t%c\t%ld\t%d\t%d\t%d\t%d",
                          &ver, &km, &ld, &nt, &rt, &nl, &rl)) < 4 ||
        (km != 'g' && km != 'f')) {
        fclose(f);
        snprintf(menu_msg, sizeof(menu_msg), "Unreadable progress file (.srs). Delete it to reset.");
        return -1;
    }
    if (ver != SRS_VERSION) {
        fclose(f);
        snprintf(menu_msg, sizeof(menu_msg), "Progress file is from a newer version (v%d).", ver);
        return -1;
    }
    if (nparsed >= 5 && rt >= 0)                         saved_reviews_today = rt;
    if (nparsed >= 6 && nl >= 0 && nl <= MAX_NEW_LIMIT)  new_limit = nl;
    if (nparsed >= 7 && rl >= 0 && rl <= MAX_REVIEW_LIMIT) review_limit = rl;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        char *tab;
        int reps, interval, idx;
        float ease;
        long due;

        if (len > 0 && line[len - 1] != '\n' && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != EOF && ch != '\n') { }
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        tab = strchr(line, '\t');
        if (!tab) { progress_bad++; continue; }
        *tab = '\0';

        if (sscanf(tab + 1, "%d %d %f %ld", &reps, &interval, &ease, &due) != 4 ||
            reps < 0 || interval < 0 || due < 0) {
            progress_bad++;
            continue;
        }
        saved_lines++;
        if (ease < 1.3f) ease = 1.3f;

        idx = find_unrestored(line, km, hint);
        if (idx < 0) continue;                 /* card no longer in the deck */
        hint = idx + 1;

        cards[idx].reps       = reps;
        cards[idx].interval   = interval;
        cards[idx].ease       = ease;
        cards[idx].due        = due;
        cards[idx].introduced = 1;
        progress_restored++;
    }
    fclose(f);

    /* Saved cards but none of them in this deck: probably the wrong file. Don't overwrite it. */
    if (saved_lines > 0 && progress_restored == 0) {
        snprintf(menu_msg, sizeof(menu_msg),
                 "Progress file doesn't match this deck. Delete the .srs to reset.");
        return -1;
    }

    last_study_day  = ld;
    saved_new_today = nt;
    return 0;
}

/* Write to <deck>.srs.tmp, then swap it in. Returns 0 on success, -1 on failure. */
static int save_progress(void) {
    char p[200], t[208];
    FILE *f;
    int i;

    srs_path_for(decks[current_deck].path, p, sizeof(p));
    snprintf(t, sizeof(t), "%s.tmp", p);

    f = fopen(t, "w");
    if (!f) return -1;

    fprintf(f, "#srs\t%d\t%c\t%ld\t%d\t%d\t%d\t%d\n", SRS_VERSION, ids_ok ? 'g' : 'f',
            base_day, new_today, reviews_today, new_limit, review_limit);
    for (i = 0; i < num_cards; i++) {
        const Card *c = &cards[i];
        if (!c->introduced) continue;
        fprintf(f, "%s\t%d\t%d\t%.2f\t%ld\n", key_of(c), c->reps, c->interval, c->ease, c->due);
    }

    if (ferror(f)) { fclose(f); sceIoRemove(t); return -1; }
    if (fclose(f) != 0) { sceIoRemove(t); return -1; }

    sceIoRemove(p);                              /* ignore error: may not exist yet */
    if (sceIoRename(t, p) < 0) return -1;        /* .tmp stays and is picked up on next load */

    unsaved = 0;
    return 0;
}

/* Save if there is something to save. Never saves while in debug "skip day" mode. */
static int try_save(void) {
    if (current_deck < 0 || day_offset != 0 || unsaved == 0) return 0;
    if (save_progress() < 0) {
        snprintf(status_msg, sizeof(status_msg), "SAVE FAILED - progress is only in memory!");
        return -1;
    }
    status_msg[0] = '\0';
    return 0;
}

/* Load a deck and its progress. Returns 1 if it can be studied, 0 otherwise (menu_msg is set). */
static int open_deck(int idx) {
    int n = load_deck(decks[idx].path);
    long real;

    if (n < 0) {
        snprintf(menu_msg, sizeof(menu_msg), "Could not open %s", decks[idx].name);
        return 0;
    }
    if (n == 0) {
        snprintf(menu_msg, sizeof(menu_msg), "No valid cards in %s (%d lines skipped)",
                 decks[idx].name, skipped_lines);
        return 0;
    }

    decide_key_mode();
    if (load_progress(decks[idx].path) < 0) return 0;

    real = get_real_day();
    base_day = real;
    clock_clamped = 0;
    if (last_study_day > real) {               /* clock is behind the last session */
        base_day = last_study_day;
        clock_clamped = 1;
    }
    day_offset = 0;
    new_today     = (last_study_day == base_day) ? saved_new_today     : 0;
    reviews_today = (last_study_day == base_day) ? saved_reviews_today : 0;
    unsaved = 0;
    status_msg[0] = '\0';
    menu_msg[0] = '\0';
    current_deck = idx;
    return 1;
}

/* ---------- SM-2 and scheduling ---------- */

/* q: 0..5 quality of recall. Below 3 counts as a lapse. */
static void sm2_review(Card *c, int q, long today) {
    if (q < 3) {
        c->reps = 0;
        c->interval = 1;
        c->due = today;          /* stays due now: shown again this session */
        return;
    }

    if (c->reps == 0)      c->interval = 1;
    else if (c->reps == 1) c->interval = 6;
    else                   c->interval = (int)(c->interval * c->ease + 0.5f);

    c->reps++;
    c->ease += 0.1f - (5 - q) * (0.08f + (5 - q) * 0.02f);
    if (c->ease < 1.3f) c->ease = 1.3f;

    c->due = today + c->interval;
}

/* Reviews left under today's limit (a huge number when there is no limit). */
static int review_left(void) {
    if (review_limit == 0) return 1 << 30;
    return review_limit > reviews_today ? review_limit - reviews_today : 0;
}

/* Due cards first, then (if today's new-card limit allows) a brand-new card.
   Learned cards (reps >= 1) are only offered while the review limit allows it;
   cards you failed today (reps == 0) always come back, they don't count as reviews.
   Avoids showing 'last' twice in a row when something else is available. */
static int pick_card(long today, int last) {
    int i, fallback = -1;
    int reviews_ok = (review_left() > 0);

    for (i = 0; i < num_cards; i++) {
        if (cards[i].introduced && cards[i].due <= today) {
            if (cards[i].reps >= 1 && !reviews_ok) continue;   /* held back by the review limit */
            if (i != last) return i;
            fallback = i;
        }
    }

    if (new_today < new_limit) {
        for (i = 0; i < num_cards; i++) {
            if (!cards[i].introduced) {
                cards[i].introduced = 1;
                new_today++;
                unsaved++;
                return i;
            }
        }
    }

    return fallback;
}

typedef struct {
    int learn;          /* new today / failed today, not passed yet: always offered */
    int review;         /* learned cards that are due (before the review limit) */
    int review_avail;   /* ... of which the review limit still allows today */
    int unseen;         /* cards never shown */
    int new_left;       /* new cards still allowed today */
    int tomorrow;       /* cards due by tomorrow (includes anything carried over) */
} Counts;

static void get_counts(long today, Counts *c) {
    int i, left = review_left();
    memset(c, 0, sizeof(*c));
    for (i = 0; i < num_cards; i++) {
        const Card *k = &cards[i];
        if (!k->introduced) { c->unseen++; continue; }
        if (k->due <= today) {
            if (k->reps == 0) c->learn++; else c->review++;
        }
        if (k->due <= today + 1) c->tomorrow++;
    }
    c->review_avail = c->review < left ? c->review : left;
    c->new_left = new_limit > new_today ? new_limit - new_today : 0;
    if (c->new_left > c->unseen) c->new_left = c->unseen;
}

/* ---------- Drawing ---------- */

static void draw_header(const char *title, const char *right) {
    char buf[200];
    float rw = right ? text_width(right, 0.8f) : 0.0f;

    fill_rect(0, 0, SCR_W, HEADER_H, COL_PANEL);
    fit_text(title, (float)SCR_W - 24.0f - rw - 16.0f, 0.95f, buf, sizeof(buf));
    draw_text(12, 21, 0.95f, COL_TEXT, INTRAFONT_ALIGN_LEFT, buf);
    if (right) draw_text(SCR_W - 12, 21, 0.8f, COL_DIM, INTRAFONT_ALIGN_RIGHT, right);
}

static void draw_footer(const char *text) {
    fill_rect(0, FOOTER_Y, SCR_W, SCR_H - FOOTER_Y, COL_PANEL);
    draw_text(10, 265, 0.7f, COL_DIM, INTRAFONT_ALIGN_LEFT, text);
}

/* Red bar with the last save error, above the footer. */
static void draw_status_bar(void) {
    char buf[200];
    if (!status_msg[0]) return;
    fill_rect(10, 224, 460, 22, COL_ERR);
    fit_text(status_msg, 440.0f, 0.8f, buf, sizeof(buf));
    draw_text(240, 239, 0.8f, COL_TEXT, INTRAFONT_ALIGN_CENTER, buf);
}

static void fmt_interval(int days, char *out, size_t n) {
    if (days < 30)        snprintf(out, n, "%d d", days);
    else if (days < 365)  snprintf(out, n, "%.1f mo", (double)days / 30.0);
    else                  snprintf(out, n, "%.1f y", (double)days / 365.0);
}

static void draw_menu(int sel, int top) {
    char right[48], buf[200];
    int i;

    snprintf(right, sizeof(right), "%d deck%s", num_decks, num_decks == 1 ? "" : "s");
    draw_header("SRS Decks", right);

    if (num_decks == 0) {
        char msg[300];
        draw_text(240, 80, 1.2f, COL_TEXT, INTRAFONT_ALIGN_CENTER, "No decks found");
        snprintf(msg, sizeof(msg),
                 "Create the folder %s/ on the memory stick and copy your deck files "
                 "into it. A deck is a text file ending in .tsv with one card per line: "
                 "the front, a tab, then the back. Press Triangle to look again.",
                 deck_dirs[0]);
        draw_block(msg, 240, 150, 380, 0.9f, 110, COL_DIM);
    } else {
        for (i = 0; i < MENU_ROWS && top + i < num_decks; i++) {
            int idx = top + i;
            float y = 36.0f + (float)i * 24.0f;
            if (idx == sel) {
                fill_rect(10, y, 450, 22, COL_CARD);
                fill_rect(10, y, 4, 22, COL_ACCENT);
            }
            fit_text(decks[idx].name, 420.0f, 1.0f, buf, sizeof(buf));
            draw_text(24, y + 16, 1.0f, idx == sel ? COL_TEXT : COL_DIM, INTRAFONT_ALIGN_LEFT, buf);
        }
        if (num_decks > MENU_ROWS) {                      /* scroll bar */
            float track = (float)MENU_ROWS * 24.0f;
            float thumb = track * (float)MENU_ROWS / (float)num_decks;
            fill_rect(468, 36, 4, track, COL_PANEL);
            fill_rect(468, 36.0f + track * (float)top / (float)num_decks, 4, thumb, COL_ACCENT);
        }
    }

    if (menu_msg[0]) {
        fill_rect(10, 228, 460, 20, COL_WARN);
        fit_text(menu_msg, 440.0f, 0.75f, buf, sizeof(buf));
        draw_text(240, 242, 0.75f, COL_TEXT, INTRAFONT_ALIGN_CENTER, buf);
    }
    draw_footer("Up/Down: choose     X: open     Triangle: look for decks again");
}

static float stat_y;
static void stat_row(const char *label, const char *value) {
    draw_text(14, stat_y, 0.72f, COL_DIM, INTRAFONT_ALIGN_LEFT, label);
    draw_text(132, stat_y, 0.72f, COL_TEXT, INTRAFONT_ALIGN_LEFT, value);
    stat_y += 13.0f;
}

static void draw_stats(void) {
    int i, young = 0, mature = 0, y, m, d;
    long today = cur_today();
    char buf[200], val[260];
    Counts c;

    get_counts(today, &c);
    for (i = 0; i < num_cards; i++) {
        if (!cards[i].introduced || cards[i].reps == 0) continue;
        if (cards[i].interval >= 21) mature++; else young++;
    }
    civil_from_days(today, &y, &m, &d);

    draw_header("Stats", day_offset ? "TEST MODE" : NULL);
    stat_y = 46.0f;

    stat_row("Deck", decks[current_deck].name);
    snprintf(val, sizeof(val), "%04d-%02d-%02d", y, m, d);
    stat_row("Study day", val);
    fit_text(decks[current_deck].path, 330.0f, 0.72f, buf, sizeof(buf));
    stat_row("File", buf);
    snprintf(val, sizeof(val), "%d loaded (max %d)", num_cards, MAX_CARDS);
    stat_row("Cards", val);
    snprintf(val, sizeof(val), "%d lines skipped, %d fields cut, %d invalid bytes",
             skipped_lines, truncated, replaced_chars);
    stat_row("Loader", val);
    if (progress_found) snprintf(val, sizeof(val), "%d cards restored, %d bad lines", progress_restored, progress_bad);
    else                snprintf(val, sizeof(val), "no file yet (new deck)");
    stat_row("Progress", val);
    stat_row("Card keys", ids_ok ? "card IDs" : "front text");
    if (clock_clamped) {
        civil_from_days(base_day, &y, &m, &d);
        snprintf(val, sizeof(val), "BEHIND last session, using %04d-%02d-%02d", y, m, d);
        stat_row("Clock", val);
    }
    if (day_offset) stat_row("Saving", "OFF (test mode)");
    stat_y += 6.0f;

    snprintf(val, sizeof(val), "%d", c.unseen);                       stat_row("Never seen", val);
    snprintf(val, sizeof(val), "%d", c.learn + c.review_avail);       stat_row("Due now", val);
    if (c.review > c.review_avail) {
        snprintf(val, sizeof(val), "%d reviews", c.review - c.review_avail);
        stat_row("Held back", val);
    }
    snprintf(val, sizeof(val), "%d", c.tomorrow);                     stat_row("Due by tomorrow", val);
    snprintf(val, sizeof(val), "%d / %d", new_today, new_limit);      stat_row("New today", val);
    if (review_limit) snprintf(val, sizeof(val), "%d / %d", reviews_today, review_limit);
    else              snprintf(val, sizeof(val), "%d (no limit)", reviews_today);
    stat_row("Reviews today", val);
    snprintf(val, sizeof(val), "%d", young);                          stat_row("Learning (<21 d)", val);
    snprintf(val, sizeof(val), "%d", mature);                         stat_row("Mature (>=21 d)", val);

    draw_status_bar();
    draw_footer("SELECT: back to the cards");
}

static void draw_settings(int row) {
    static const char *const labels[2] = { "New cards per day", "Reviews per day" };
    static const char *const notes[2]  = { "Cards you have never seen before.  0 = no new cards",
                                           "Answers on cards you already learned.  0 = no limit" };
    char val[32], buf[160];
    int r;

    draw_header("Daily limits", decks[current_deck].name);
    if (day_offset) fill_rect(0, HEADER_H, SCR_W, 3, COL_WARN);

    for (r = 0; r < 2; r++) {
        float y = 42.0f + (float)r * 52.0f;
        int v = (r == 0) ? new_limit : review_limit;

        fill_rect(24, y, 432, 46, r == row ? COL_CARD : COL_PANEL);
        if (r == row) fill_rect(24, y, 4, 46, COL_ACCENT);
        draw_text(40, y + 20, 1.0f, r == row ? COL_TEXT : COL_DIM, INTRAFONT_ALIGN_LEFT, labels[r]);
        draw_text(40, y + 37, 0.7f, COL_DIM, INTRAFONT_ALIGN_LEFT, notes[r]);
        if (r == row) snprintf(val, sizeof(val), "<  %d  >", v);
        else          snprintf(val, sizeof(val), "%d", v);
        draw_text(440, y + 30, 1.5f, r == row ? COL_ACCENT : COL_TEXT, INTRAFONT_ALIGN_RIGHT, val);
    }

    snprintf(buf, sizeof(buf), "Today so far: %d new cards, %d reviews", new_today, reviews_today);
    draw_text(24, 160, 0.85f, COL_TEXT, INTRAFONT_ALIGN_LEFT, buf);
    draw_para("Cards you fail today keep coming back and do not count against the review "
              "limit. Limits are saved separately for each deck.",
              24, 182, 430, 0.75f, 3, COL_DIM);
    if (day_offset)
        draw_text(24, 232, 0.8f, COL_WARN, INTRAFONT_ALIGN_LEFT, "TEST MODE: nothing is saved.");

    draw_status_bar();
    draw_footer("Up/Down: choose     Left/Right: change (hold = faster)     L: back");
}

/* The "nothing left for today" notice, with the reason and today's totals. */
static void draw_done(const Counts *c) {
    char val[64], hint[420], tmp[220];
    int blocked = c->review - c->review_avail;
    const char *title = (blocked > 0) ? "Daily review limit reached"
                                      : "No more cards due today";

    fill_rect(24, 38, 432, 204, COL_CARD);
    fill_rect(24, 38, 432, 4, blocked > 0 ? COL_WARN : COL_OK);
    draw_text(240, 70, 1.25f, COL_TEXT, INTRAFONT_ALIGN_CENTER, title);
    if (blocked == 0 && c->unseen > 0 && c->new_left == 0)
        draw_text(240, 87, 0.8f, COL_WARN, INTRAFONT_ALIGN_CENTER, "New-card limit reached");

    if (review_limit) snprintf(val, sizeof(val), "%d / %d", reviews_today, review_limit);
    else              snprintf(val, sizeof(val), "%d (no limit)", reviews_today);
    draw_text(70, 112, 0.9f, COL_DIM, INTRAFONT_ALIGN_LEFT, "Reviews today");
    draw_text(410, 112, 0.9f, COL_TEXT, INTRAFONT_ALIGN_RIGHT, val);

    snprintf(val, sizeof(val), "%d / %d", new_today, new_limit);
    draw_text(70, 130, 0.9f, COL_DIM, INTRAFONT_ALIGN_LEFT, "New cards today");
    draw_text(410, 130, 0.9f, COL_TEXT, INTRAFONT_ALIGN_RIGHT, val);

    snprintf(val, sizeof(val), "%d", c->tomorrow);
    draw_text(70, 148, 0.9f, COL_DIM, INTRAFONT_ALIGN_LEFT, "Due tomorrow");
    draw_text(410, 148, 0.9f, COL_TEXT, INTRAFONT_ALIGN_RIGHT, val);

    hint[0] = '\0';
    if (blocked > 0) {
        snprintf(tmp, sizeof(tmp),
                 "%d reviews are still due and carry over to tomorrow. Want more today? "
                 "Press L, choose \"Reviews per day\" and press Right to raise it (0 = no limit). ",
                 blocked);
        strcat(hint, tmp);
    }
    if (c->unseen > 0 && c->new_left == 0) {
        snprintf(tmp, sizeof(tmp),
                 "%d new cards are waiting in this deck. Want more today? "
                 "Press L, choose \"New cards per day\" and press Right to raise it.",
                 c->unseen);
        strcat(hint, tmp);
    }
    if (blocked == 0 && c->unseen == 0)
        snprintf(hint, sizeof(hint),
                 "Every card in this deck has been introduced. Come back tomorrow for the next reviews.");

    draw_block(hint, 240, 200, 392, 0.85f, 70, COL_TEXT);
}

typedef struct {
    const char  *label;
    const char  *key;
    int          q;
    unsigned int color;
} RatingBtn;

static const RatingBtn rating_btn[4] = {
    { "Again", "Square",   1, COL_AGAIN },
    { "Hard",  "Triangle", 3, COL_HARD  },
    { "Good",  "Circle",   4, COL_GOOD  },
    { "Easy",  "X",        5, COL_EASY  }
};

static const char *card_kind(const Card *c) {
    if (c->reps == 0 && c->interval == 0) return "NEW";
    if (c->reps == 0)                     return "RELEARN";
    return "REVIEW";
}

static void draw_card_screen(int current, int revealed) {
    long today = cur_today();
    Counts c;
    char right[120], tmp[120];

    get_counts(today, &c);
    if (review_limit)
        snprintf(right, sizeof(right), "Due %d    New %d    Rev %d/%d",
                 c.learn + c.review_avail, c.new_left, reviews_today, review_limit);
    else
        snprintf(right, sizeof(right), "Due %d    New %d    Rev %d",
                 c.learn + c.review_avail, c.new_left, reviews_today);
    if (day_offset) {
        snprintf(tmp, sizeof(tmp), "TEST +%ld d    %s", day_offset, right);
        snprintf(right, sizeof(right), "%s", tmp);
    }
    draw_header(decks[current_deck].name, right);
    if (day_offset) fill_rect(0, HEADER_H, SCR_W, 3, COL_WARN);

    if (current < 0) {
        draw_done(&c);
    } else {
        const Card *k = &cards[current];
        const char *kind = card_kind(k);
        unsigned int kcol = (k->reps >= 1) ? COL_OK : (k->interval == 0 ? COL_ACCENT : COL_WARN);

        fill_rect(24, 38, 432, 150, COL_CARD);
        draw_text(36, 56, 0.7f, kcol, INTRAFONT_ALIGN_LEFT, kind);
        snprintf(tmp, sizeof(tmp), "reps %d   interval %d d   ease %.2f", k->reps, k->interval, (double)k->ease);
        draw_text(444, 56, 0.65f, COL_DIM, INTRAFONT_ALIGN_RIGHT, tmp);

        if (!revealed) {
            draw_block(k->front, 240, 112, 396, 1.6f, 112, COL_TEXT);
            fill_rect(24, 196, 432, 50, COL_ACCENT);
            draw_text(240, 227, 1.0f, COL_TEXT, INTRAFONT_ALIGN_CENTER, "Show answer      (press X)");
        } else {
            int i;
            draw_block(k->front, 240, 80, 396, 1.0f, 36, COL_DIM);
            fill_rect(48, 101, 384, 1, COL_DIM);
            draw_block(k->back, 240, 146, 396, 1.4f, 78, COL_TEXT);

            for (i = 0; i < 4; i++) {
                float x = 24.0f + (float)i * 110.0f, w = 102.0f;
                char iv[24];
                if (rating_btn[i].q < 3) {
                    snprintf(iv, sizeof(iv), "again soon");
                } else {
                    Card t = *k;                      /* what would this answer do? */
                    sm2_review(&t, rating_btn[i].q, today);
                    fmt_interval(t.interval, iv, sizeof(iv));
                }
                fill_rect(x, 196, w, 50, rating_btn[i].color);
                draw_text(x + w / 2, 212, 0.95f, COL_TEXT, INTRAFONT_ALIGN_CENTER, rating_btn[i].label);
                draw_text(x + w / 2, 227, 0.75f, COL_TEXT, INTRAFONT_ALIGN_CENTER, iv);
                draw_text(x + w / 2, 241, 0.65f, RGBA(255, 255, 255, 170), INTRAFONT_ALIGN_CENTER, rating_btn[i].key);
            }
        }
    }

    draw_status_bar();
    draw_footer("L: limits     SELECT: stats     START: deck list     R: skip a day (test)");
}

/* ---------- Main loop ---------- */

enum { MODE_MENU, MODE_REVIEW };

int main(void) {
    SceCtrlData pad;
    unsigned int old = 0, pressed;
    unsigned int frame = 0;
    int mode = MODE_MENU;
    int sel = 0, top = 0;
    int current = -1, last = -1;
    int revealed = 0, show_stats = 0, show_settings = 0, setting_row = 0;
    int leave_armed = 0;
    int hold_up = 0, hold_down = 0, hold_left = 0, hold_right = 0;

    setup_callbacks();
    gu_setup();

    intraFontInit();
    /* UTF-8 strings (accents) and the larger glyph cache. ltn8 is a firmware sans-serif font. */
    font = intraFontLoad("flash0:/font/ltn8.pgf", INTRAFONT_STRING_UTF8 | INTRAFONT_CACHE_LARGE);
    if (!font)
        font = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_STRING_UTF8 | INTRAFONT_CACHE_LARGE);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    sceIoMkdir(deck_dirs[0], 0777);   /* create the folder on first run (harmless if it exists) */
    scan_decks();

    while (!quit_requested) {
        sceCtrlReadBufferPositive(&pad, 1);
        pressed = pad.Buttons & ~old;   /* only newly pressed buttons */
        old = pad.Buttons;

        if (mode == MODE_MENU) {
            if (num_decks > 0) {
                if (key_repeat(&hold_down, (pad.Buttons & PSP_CTRL_DOWN) != 0, (pressed & PSP_CTRL_DOWN) != 0))
                    sel = (sel + 1) % num_decks;
                if (key_repeat(&hold_up, (pad.Buttons & PSP_CTRL_UP) != 0, (pressed & PSP_CTRL_UP) != 0))
                    sel = (sel + num_decks - 1) % num_decks;

                if (sel < top)              top = sel;
                if (sel >= top + MENU_ROWS) top = sel - MENU_ROWS + 1;

                if (pressed & PSP_CTRL_CROSS) {
                    if (open_deck(sel)) {
                        mode = MODE_REVIEW;
                        last = -1;
                        revealed = 0;
                        show_stats = 0;
                        show_settings = 0;
                        leave_armed = 0;
                        current = pick_card(cur_today(), last);
                    }
                }
            }

            if (pressed & PSP_CTRL_TRIANGLE) {
                scan_decks();
                sel = 0;
                top = 0;
                menu_msg[0] = '\0';
            }
        } else {
            /* Check the clock about twice a second: a new study day starts at 04:00. */
            if ((frame++ & 31) == 0) {
                long real = get_real_day();
                if (real > base_day) {
                    base_day = real;
                    new_today = 0;
                    reviews_today = 0;
                    if (current < 0 && day_offset == 0) {
                        current = pick_card(cur_today(), -1);
                        revealed = 0;
                    }
                }
            }

            if (pressed & PSP_CTRL_START) {      /* back to the deck list */
                if (day_offset != 0) {
                    snprintf(menu_msg, sizeof(menu_msg), "Test mode: progress was not saved.");
                    mode = MODE_MENU;
                } else if (try_save() == 0 || leave_armed) {
                    menu_msg[0] = '\0';
                    mode = MODE_MENU;
                } else {
                    leave_armed = 1;             /* save failed: a second START leaves anyway */
                    snprintf(status_msg, sizeof(status_msg),
                             "SAVE FAILED - press START again to leave WITHOUT saving");
                }
            } else {
                /* L: open / close the limits screen. Closing saves and may free up more cards. */
                if (pressed & PSP_CTRL_LTRIGGER) {
                    show_settings = !show_settings;
                    if (show_settings) {
                        show_stats = 0;
                        setting_row = 0;
                    } else {
                        try_save();
                        if (current < 0) {
                            current = pick_card(cur_today(), last);
                            revealed = 0;
                        }
                    }
                }

                if ((pressed & PSP_CTRL_SELECT) && !show_settings)
                    show_stats = !show_stats;

                if (pressed & PSP_CTRL_RTRIGGER) {
                    if (day_offset == 0) try_save();   /* keep real progress before entering test mode */
                    day_offset++;
                    new_today = 0;
                    reviews_today = 0;
                    last = -1;
                    if (!show_settings) {
                        current = pick_card(cur_today(), last);
                        revealed = 0;
                    }
                }

                if (show_settings) {
                    int fire_l, fire_r, step, delta = 0;

                    if (pressed & PSP_CTRL_UP)   setting_row = 0;
                    if (pressed & PSP_CTRL_DOWN) setting_row = 1;

                    fire_l = key_repeat(&hold_left,  (pad.Buttons & PSP_CTRL_LEFT)  != 0, (pressed & PSP_CTRL_LEFT)  != 0);
                    fire_r = key_repeat(&hold_right, (pad.Buttons & PSP_CTRL_RIGHT) != 0, (pressed & PSP_CTRL_RIGHT) != 0);
                    step = (hold_left > 90 || hold_right > 90) ? 10 : 1;   /* hold longer = bigger steps */
                    if (fire_r && !fire_l) delta = step;
                    if (fire_l && !fire_r) delta = -step;

                    if (delta) {
                        int *v   = (setting_row == 0) ? &new_limit : &review_limit;
                        int maxv = (setting_row == 0) ? MAX_NEW_LIMIT : MAX_REVIEW_LIMIT;
                        *v += delta;
                        if (*v < 0)    *v = 0;
                        if (*v > maxv) *v = maxv;
                        unsaved++;
                    }
                } else if (!show_stats && current >= 0) {
                    if (!revealed) {
                        if (pressed & PSP_CTRL_CROSS) revealed = 1;
                    } else {
                        int q = -1;
                        if      (pressed & PSP_CTRL_SQUARE)   q = 1;
                        else if (pressed & PSP_CTRL_TRIANGLE) q = 3;
                        else if (pressed & PSP_CTRL_CIRCLE)   q = 4;
                        else if (pressed & PSP_CTRL_CROSS)    q = 5;

                        if (q >= 0) {
                            if (cards[current].reps >= 1) reviews_today++;   /* a learned card: counts as a review */
                            sm2_review(&cards[current], q, cur_today());
                            unsaved++;
                            last = current;
                            current = pick_card(cur_today(), last);
                            revealed = 0;
                            if (unsaved >= SAVE_EVERY || current < 0) try_save();
                        }
                    }
                }
            }
        }

        /* Draw the whole screen every frame (the GPU swaps between two buffers). */
        begin_frame();
        if (mode == MODE_MENU)    draw_menu(sel, top);
        else if (show_settings)   draw_settings(setting_row);
        else if (show_stats)      draw_stats();
        else                      draw_card_screen(current, revealed);
        end_frame();
    }

    /* HOME was confirmed: save, then leave. */
    if (mode == MODE_REVIEW) try_save();
    if (font) intraFontUnload(font);
    intraFontShutdown();
    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
