#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <input/input.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_poller.h>
#include <nfc/nfc_listener.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <storage/storage.h>

// Set to 0 for UI-only diagnostic build (Read Card never scans).
#define ENABLE_WORKER 1

#define TAG "CreatureScanner"
#define FOLDER          "/ext/nfc/creatures"
#define NAME_START_PAGE 207   // ASCII phrase begins here (after 2 leading 0x00 bytes)
#define NAME_END_PAGE   213   // inclusive, phrase terminated by 0x00 padding
#define TOTAL_PAGES     231   // NTAG216
#define MAX_FILES       128   // playback list capacity (heap-allocated)

#define READ_DONE_FLAG     (1u << 0)
#define READ_FAIL_FLAG     (1u << 1)
#define OVERWRITE_YES_FLAG (1u << 2)
#define OVERWRITE_NO_FLAG  (1u << 3)
#define CONFIRM_YES_FLAG   (1u << 4)
#define CONFIRM_NO_FLAG    (1u << 5)
#define CANCEL_FLAG        (1u << 6)
#define PB_NEXT_FLAG       (1u << 7)
#define PB_PREV_FLAG       (1u << 8)
#define PB_DEL_YES_FLAG    (1u << 9)
#define PB_DEL_NO_FLAG     (1u << 10)
#define PB_EXIT_FLAG       (1u << 11)

typedef enum {
    ScanStateWaiting,
    ScanStateReading,
    ScanStateSaving,
    ScanStateDone,
    ScanStateAskOverwrite,
    ScanStateConfirmSave,
    ScanStateFailed
} ScanState;

typedef enum { ViewSubmenu, ViewSettings, ViewAbout, ViewScan, ViewPlayback } AppView;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    VariableItemList* variable_item_list;
    Widget* widget_about;
    View* view_scan;
    View* view_playback;
    Nfc* nfc;
    NfcListener* listener;   // live while emulating
    NfcDevice* emu_device;   // parsed .nfc file, lives while the listener exists
    FuriEventFlag* events;
    FuriThread* worker;
    FuriThread* playback_worker;
    char (*files)[64];       // heap: creature file names (without .nfc)
    uint8_t file_count;
    uint8_t file_index;
    bool running;
    bool scanning;        // true while the Read view is active
    AppView current_view; // which view is on screen (for Back handling)
    bool mode_slow;       // slow mode: confirm each save
    bool detect_only;     // poller only detects card presence (removal check)
    bool card_present;    // last probe result (removal check)
    bool emulating;       // true while playback emulation runs
    char name[64];
    char filename[128];
    char status[128];
} App;

#if ENABLE_WORKER
typedef struct {
    uint8_t pages[TOTAL_PAGES][4];
    uint8_t uid[7];
    uint8_t signature[32];
    size_t pages_read;
    bool ok;
} TagData;

static TagData tag_data;
#endif

// Scan view model (accessed with with_view_model for thread safety)
typedef struct {
    ScanState state;
} ScanModel;

// Playback view model
typedef struct {
    char name[64];     // currently emulated card name
    uint8_t index;     // 1-based for display
    uint8_t count;     // total files
    bool confirm;      // delete confirmation dialog visible
    bool no_files;     // folder empty / unreadable
    bool emu_failed;   // emulation could not be started
    char emu_error[64];
} PlaybackModel;

// ---------------------------------------------------------------- Helpers
static void vibro_pulse(uint32_t ms) {
    furi_hal_vibro_on(true);
    furi_delay_ms(ms);
    furi_hal_vibro_on(false);
}

// ---------------------------------------------------------------- Scan view
static void scan_view_draw_cb(Canvas* canvas, void* ctx) {
    ScanModel* model = ctx;
    canvas_clear(canvas);

    canvas_draw_str(canvas, 4, 12, "Read Card");
    canvas_draw_line(canvas, 0, 16, 127, 16);

    switch(model->state) {
    case ScanStateWaiting:
        canvas_draw_str(canvas, 8, 34, "Waiting for card...");
        canvas_draw_str(canvas, 8, 46, "Hold it against the back");
        break;
    case ScanStateReading:
        canvas_draw_str(canvas, 8, 34, "Reading card...");
        break;
    case ScanStateSaving:
        canvas_draw_str(canvas, 8, 34, "Saving...");
        break;
    case ScanStateDone:
        canvas_draw_str(canvas, 8, 28, "Saved.");
        canvas_draw_str(canvas, 8, 46, "Scanning for next card...");
        break;
    case ScanStateAskOverwrite:
        canvas_draw_str(canvas, 8, 28, "File already exists.");
        canvas_draw_str(canvas, 8, 40, "Overwrite it?");
        elements_button_left(canvas, "Skip");
        elements_button_right(canvas, "Overwrite");
        break;
    case ScanStateConfirmSave:
        canvas_draw_str(canvas, 8, 28, "Save this card?");
        elements_button_left(canvas, "Discard");
        elements_button_right(canvas, "Save");
        break;
    case ScanStateFailed:
        canvas_draw_str(canvas, 8, 28, "Failed:");
        canvas_draw_str(canvas, 8, 42, "Retrying...");
        break;
    }
}

#if ENABLE_WORKER
static void scan_view_set_state(App* app, ScanState state) {
    with_view_model(
        app->view_scan, ScanModel * model, { model->state = state; }, true);
}
#endif

static ScanState scan_view_get_state(App* app) {
    ScanState state = ScanStateWaiting;
    with_view_model(
        app->view_scan, ScanModel * model, { state = model->state; }, false);
    return state;
}

static bool scan_view_input_cb(InputEvent* event, void* ctx) {
    App* app = ctx;
    if(event->type != InputTypeShort && event->type != InputTypeLong) {
        return false;
    }

    ScanState state = scan_view_get_state(app);

    switch(state) {
    case ScanStateAskOverwrite:
        if(event->key == InputKeyLeft) {
            furi_event_flag_set(app->events, OVERWRITE_NO_FLAG);
            vibro_pulse(30);
            return true;
        } else if(event->key == InputKeyRight) {
            furi_event_flag_set(app->events, OVERWRITE_YES_FLAG);
            vibro_pulse(30);
            return true;
        }
        break;

    case ScanStateConfirmSave:
        if(event->key == InputKeyLeft) {
            furi_event_flag_set(app->events, CONFIRM_NO_FLAG);
            vibro_pulse(30);
            return true;
        } else if(event->key == InputKeyRight) {
            furi_event_flag_set(app->events, CONFIRM_YES_FLAG);
            vibro_pulse(30);
            return true;
        }
        break;

    default:
        break;
    }

    return false;
}

// ---------------------------------------------------------------- Playback view
static void playback_view_draw_cb(Canvas* canvas, void* ctx) {
    PlaybackModel* model = ctx;
    canvas_clear(canvas);

    canvas_draw_str(canvas, 4, 12, "Playback");
    canvas_draw_line(canvas, 0, 16, 127, 16);

    if(model->no_files) {
        canvas_draw_str(canvas, 8, 34, "No cards found in");
        canvas_draw_str(canvas, 8, 46, FOLDER);
        return;
    }

    if(model->confirm) {
        canvas_draw_str(canvas, 8, 28, "Delete this card?");
        canvas_draw_str(canvas, 8, 40, model->name);
        elements_button_left(canvas, "Cancel");
        elements_button_right(canvas, "Delete");
        return;
    }

    // Counter in the top right corner, right-aligned
    char pos[20];
    snprintf(pos, sizeof(pos), "%u/%u", model->index, model->count);
    canvas_draw_str_aligned(canvas, 124, 12, AlignRight, AlignBottom, pos);

    // Emulation status. Emulation answers a reader's field, like a real card.
    if(model->emu_failed) {
        canvas_draw_str(canvas, 8, 32, "Emulation error:");
        canvas_draw_str(canvas, 8, 44, model->emu_error);
    } else {
        canvas_draw_str(canvas, 8, 32, "Emulating:");
        canvas_draw_str(canvas, 8, 44, model->name);
    }

    // Soft buttons: cycle on the sides, delete centered at the bottom
    elements_button_left(canvas, "Prev");
    elements_button_right(canvas, "Next");
    elements_button_center(canvas, "Delete");
}

static void playback_view_set(App* app, const char* name, uint8_t index, uint8_t count,
                              bool confirm, bool no_files) {
    with_view_model(
        app->view_playback,
        PlaybackModel * model,
        {
            if(name) strlcpy(model->name, name, sizeof(model->name));
            if(index) model->index = index;
            if(count) model->count = count;
            model->confirm = confirm;
            model->no_files = no_files;
        },
        true);
}

static void playback_view_set_emu_error(App* app, const char* error) {
    with_view_model(
        app->view_playback,
        PlaybackModel * model,
        {
            strlcpy(model->emu_error, error, sizeof(model->emu_error));
            model->emu_failed = true;
        },
        true);
}

static bool playback_view_input_cb(InputEvent* event, void* ctx) {
    App* app = ctx;
    if(event->type != InputTypeShort && event->type != InputTypeLong) {
        return false;
    }

    bool confirm = false;
    with_view_model(
        app->view_playback, PlaybackModel * model, { confirm = model->confirm; }, false);

    if(confirm) {
        if(event->key == InputKeyLeft) {
            furi_event_flag_set(app->events, PB_DEL_NO_FLAG);
            vibro_pulse(30);
            return true;
        } else if(event->key == InputKeyRight) {
            furi_event_flag_set(app->events, PB_DEL_YES_FLAG);
            vibro_pulse(30);
            return true;
        }
    } else {
        if(event->key == InputKeyLeft) {
            furi_event_flag_set(app->events, PB_PREV_FLAG);
            vibro_pulse(30);
            return true;
        } else if(event->key == InputKeyRight) {
            furi_event_flag_set(app->events, PB_NEXT_FLAG);
            vibro_pulse(30);
            return true;
        } else if(event->key == InputKeyOk || event->key == InputKeyUp) {
            playback_view_set(app, NULL, 0, 0, true, false);
            vibro_pulse(30);
            return true;
        }
    }

    return false;
}

// ------------------------------------------------- Playback file handling
static void playback_list_files(App* app) {
    FURI_LOG_I(TAG, "list: opening " FOLDER);
    app->file_count = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* dir = storage_file_alloc(storage);

    if(storage_dir_open(dir, FOLDER)) {
        FileInfo info;
        char name[256];
        while(app->file_count < MAX_FILES &&
              storage_dir_read(dir, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;

            size_t len = strlen(name);
            if(len > 4 && strcmp(name + len - 4, ".nfc") == 0) {
                size_t copy = len - 4;
                if(copy > 63) copy = 63;
                memcpy(app->files[app->file_count], name, copy);
                app->files[app->file_count][copy] = '\0';
                app->file_count++;
            }
        }
        storage_dir_close(dir);
    } else {
        FURI_LOG_E(TAG, "list: dir open failed");
    }
    storage_file_free(dir);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "list: %u files", app->file_count);
}

// Listener event callback: keep the emulation alive (NfcCommandContinue).
static NfcCommand playback_listener_cb(NfcGenericEvent event, void* ctx) {
    UNUSED(event);
    UNUSED(ctx);
    return NfcCommandContinue;
}

static void playback_stop_emulation(App* app) {
    if(app->emulating) {
        FURI_LOG_I(TAG, "emu: stop");
        nfc_listener_stop(app->listener);
        nfc_listener_free(app->listener);
        app->listener = NULL;
        if(app->emu_device) {
            nfc_device_free(app->emu_device);
            app->emu_device = NULL;
        }
        app->emulating = false;
    }
}

// Load the file at file_index and start emulating it.
// Uses the SDK's own .nfc parser (NfcDevice) — same path the stock NFC
// app takes before emulating — instead of hand-building the structs.
static bool playback_start_emulation(App* app) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.nfc", FOLDER, app->files[app->file_index]);

    playback_stop_emulation(app);

    // Parse the dump with the firmware's own loader
    app->emu_device = nfc_device_alloc();
    if(!app->emu_device) {
        playback_view_set_emu_error(app, "device alloc failed");
        return false;
    }

    FURI_LOG_I(TAG, "emu: loading %s", path);
    if(!nfc_device_load(app->emu_device, path)) {
        FURI_LOG_E(TAG, "emu: load failed: %s", path);
        playback_view_set_emu_error(app, "file load failed");
        nfc_device_free(app->emu_device);
        app->emu_device = NULL;
        return false;
    }

    // Verify it is the protocol we can emulate
    NfcProtocol protocol = nfc_device_get_protocol(app->emu_device);
    if(protocol != NfcProtocolMfUltralight) {
        FURI_LOG_E(TAG, "emu: wrong protocol %d", protocol);
        playback_view_set_emu_error(app, "not a NTAG dump");
        nfc_device_free(app->emu_device);
        app->emu_device = NULL;
        return false;
    }

    const MfUltralightData* data =
        nfc_device_get_data(app->emu_device, NfcProtocolMfUltralight);
    if(!data) {
        playback_view_set_emu_error(app, "no tag data");
        nfc_device_free(app->emu_device);
        app->emu_device = NULL;
        return false;
    }

    FURI_LOG_I(
        TAG,
        "emu: type=%d pages %zu/%zu uid_len=%u",
        data->type,
        data->pages_read,
        data->pages_total,
        data->iso14443_3a_data->uid_len);

    // Start emulation with the properly parsed data (listener copies it)
    FURI_LOG_I(TAG, "emu: listener_alloc");
    app->listener = nfc_listener_alloc(app->nfc, NfcProtocolMfUltralight, data);
    if(!app->listener) {
        FURI_LOG_E(TAG, "emu: listener_alloc failed");
        playback_view_set_emu_error(app, "listener alloc failed");
        nfc_device_free(app->emu_device);
        app->emu_device = NULL;
        return false;
    }
    FURI_LOG_I(TAG, "emu: listener_start");
    nfc_listener_start(app->listener, playback_listener_cb, app);
    app->emulating = true;
    return true;
}

// ---------------------------------------------------------------- Playback worker
static int32_t playback_worker(void* ctx) {
    App* app = ctx;

    while(app->running) {
        if(app->current_view != ViewPlayback) {
            if(app->emulating) playback_stop_emulation(app);
            furi_delay_ms(50);
            continue;
        }

        if(app->file_count == 0) {
            FURI_LOG_I(TAG, "worker: entering playback");
            playback_list_files(app);
            if(app->file_index >= app->file_count) app->file_index = 0;
            if(app->file_count == 0) {
                playback_view_set(app, "", 0, 0, false, true);
                furi_delay_ms(200);
                continue;
            }
        }

        if(!app->emulating) {
            if(playback_start_emulation(app)) {
                playback_view_set(
                    app,
                    app->files[app->file_index],
                    app->file_index + 1,
                    app->file_count,
                    false,
                    false);
            }
        }

        uint32_t flags = furi_event_flag_wait(
            app->events,
            PB_NEXT_FLAG | PB_PREV_FLAG | PB_DEL_YES_FLAG | PB_DEL_NO_FLAG | PB_EXIT_FLAG,
            FuriFlagWaitAny,
            200);
        if(flags & FuriFlagError) continue;

        if(flags & PB_EXIT_FLAG) {
            playback_stop_emulation(app);
            continue;
        }

        if(flags & PB_DEL_NO_FLAG) {
            playback_view_set(
                app,
                app->files[app->file_index],
                app->file_index + 1,
                app->file_count,
                false,
                false);
        }

        if(flags & (PB_NEXT_FLAG | PB_PREV_FLAG | PB_DEL_YES_FLAG)) {
            playback_stop_emulation(app);

            if(flags & PB_DEL_YES_FLAG) {
                char path[128];
                snprintf(path, sizeof(path), "%s/%s.nfc", FOLDER, app->files[app->file_index]);
                Storage* storage = furi_record_open(RECORD_STORAGE);
                storage_common_remove(storage, path);
                furi_record_close(RECORD_STORAGE);
                playback_list_files(app);
                if(app->file_index >= app->file_count) {
                    app->file_index = app->file_count ? app->file_count - 1 : 0;
                }
                vibro_pulse(80);
            } else if(flags & PB_NEXT_FLAG) {
                if(app->file_count) {
                    app->file_index = (app->file_index + 1) % app->file_count;
                }
            } else if(flags & PB_PREV_FLAG) {
                if(app->file_count) {
                    app->file_index = (app->file_index + app->file_count - 1) % app->file_count;
                }
            }
        }
    }

    playback_stop_emulation(app);
    return 0;
}

// Global navigation callback (Back key)
static bool app_navigation_cb(void* ctx) {
    App* app = ctx;

    if(app->current_view == ViewSettings || app->current_view == ViewAbout ||
       app->current_view == ViewPlayback || app->current_view == ViewScan) {
        if(app->current_view == ViewScan) {
            app->scanning = false;
            furi_event_flag_set(app->events, CANCEL_FLAG);
        }
        if(app->current_view == ViewPlayback) {
            furi_event_flag_set(app->events, PB_EXIT_FLAG);
        }
        app->current_view = ViewSubmenu;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSubmenu);
        return true;
    }

    view_dispatcher_stop(app->view_dispatcher);
    return true;
}

#if ENABLE_WORKER
// ---------------------------------------------------------------- Name extraction
static bool extract_name(const uint8_t pages[TOTAL_PAGES][4], char* out, size_t out_len) {
    size_t len = 0;
    for(int p = NAME_START_PAGE; p <= NAME_END_PAGE && len < out_len - 1; p++) {
        for(int b = 0; b < 4; b++) {
            uint8_t c = pages[p][b];
            if(c == 0x00) continue;
            if(c < 0x20 || c > 0x7E) continue;
            out[len++] = (char)c;
        }
    }
    out[len] = '\0';
    return len > 0;
}

static void sanitize(const char* in, char* out, size_t out_len) {
    size_t o = 0;
    for(size_t i = 0; in[i] && o < out_len - 1; i++) {
        char c = in[i];
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == ' ') {
            out[o++] = c;
        } else {
            out[o++] = '_';
        }
    }
    out[o] = '\0';
}

// ---------------------------------------------------------------- .nfc writer
static bool write_nfc_file(const char* path, const uint8_t uid[7],
                           const uint8_t signature[32],
                           const uint8_t pages[TOTAL_PAGES][4]) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    bool ok = false;

    if(storage_file_open(f, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* s = furi_string_alloc();

        furi_string_cat_printf(s, "Filetype: Flipper NFC device\n");
        furi_string_cat_printf(s, "Version: 4\n");
        furi_string_cat_printf(s, "# Device type can be ISO14443-3A, ISO14443-3B, "
                                  "ISO14443-4A, ISO14443-4B, ISO15693-3, FeliCa, "
                                  "NTAG/Ultralight, Mifare Classic, Mifare Plus, "
                                  "Mifare DESFire, SLIX, ST25TB, NTAG4xx, Type 4 Tag, EMV\n");
        furi_string_cat_printf(s, "Device type: NTAG/Ultralight\n");
        furi_string_cat_printf(s, "# UID is common for all formats\n");
        furi_string_cat_printf(s, "UID: %02X %02X %02X %02X %02X %02X %02X\n",
                               uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
        furi_string_cat_printf(s, "# ISO14443-3A specific data\n");
        furi_string_cat_printf(s, "ATQA: 00 44\nSAK: 00\n");
        furi_string_cat_printf(s, "# NTAG/Ultralight specific data\n");
        furi_string_cat_printf(s, "Data format version: 2\n");
        furi_string_cat_printf(s, "NTAG/Ultralight type: NTAG216\n");

        furi_string_cat_printf(s, "Signature:");
        for(int i = 0; i < 32; i++) {
            furi_string_cat_printf(s, " %02X", signature[i]);
        }
        furi_string_cat_printf(s, "\n");

        furi_string_cat_printf(s, "Mifare version: 00 04 04 02 01 00 13 03\n");
        for(int c = 0; c < 3; c++) {
            furi_string_cat_printf(s, "Counter %d: 0\n", c);
            furi_string_cat_printf(s, "Tearing %d: BD\n", c);
        }
        furi_string_cat_printf(s, "Pages total: %d\n", TOTAL_PAGES);
        furi_string_cat_printf(s, "Pages read: %d\n", TOTAL_PAGES);
        for(int p = 0; p < TOTAL_PAGES; p++) {
            furi_string_cat_printf(s, "Page %d: %02X %02X %02X %02X\n",
                                   p, pages[p][0], pages[p][1], pages[p][2], pages[p][3]);
        }
        furi_string_cat_printf(s, "Failed authentication attempts: 0\n");

        ok = storage_file_write(f, furi_string_get_cstr(s), furi_string_size(s)) ==
             furi_string_size(s);
        furi_string_free(s);
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

// ---------------------------------------------------------------- Poller
static NfcCommand poller_callback(NfcGenericEvent event, void* ctx) {
    App* app = ctx;
    MfUltralightPoller* poller = event.instance;
    const MfUltralightPollerEvent* mfu_event = event.event_data;

    if(mfu_event->type != MfUltralightPollerEventTypeRequestMode) {
        return NfcCommandContinue;
    }

    if(app->detect_only) {
        app->card_present = true;
        furi_event_flag_set(app->events, READ_DONE_FLAG);
        return NfcCommandStop;
    }

    mfu_event->data->poller_mode = MfUltralightPollerModeRead;

    scan_view_set_state(app, ScanStateReading);

    bool all_ok = true;
    tag_data.pages_read = 0;

    for(uint16_t start = 0; start < TOTAL_PAGES; start += 4) {
        MfUltralightPageReadCommandData rd;
        if(mf_ultralight_poller_read_page(poller, start, &rd) != MfUltralightErrorNone) {
            all_ok = false;
            break;
        }
        for(int i = 0; i < 4 && (start + i) < TOTAL_PAGES; i++) {
            memcpy(tag_data.pages[start + i], rd.page[i].data, 4);
            tag_data.pages_read++;
        }
    }

    MfUltralightSignature sig;
    memset(&sig, 0, sizeof(sig));
    if(mf_ultralight_poller_read_signature(poller, &sig) != MfUltralightErrorNone) {
        memset(tag_data.signature, 0, sizeof(tag_data.signature));
    } else {
        memcpy(tag_data.signature, sig.data, 32);
    }

    if(tag_data.pages_read >= 3) {
        memcpy(tag_data.uid, tag_data.pages[0], 3);
        memcpy(tag_data.uid + 3, tag_data.pages[1], 3);
        tag_data.uid[6] = tag_data.pages[2][0];
    } else {
        all_ok = false;
    }

    tag_data.ok = all_ok;
    furi_event_flag_set(app->events, all_ok ? READ_DONE_FLAG : READ_FAIL_FLAG);
    return NfcCommandStop;
}

// ---------------------------------------------------------------- Card removal
static void wait_for_card_removal(App* app) {
    app->detect_only = true;

    while(app->running && app->scanning) {
        app->card_present = false;
        furi_event_flag_clear(app->events, READ_DONE_FLAG);

        NfcPoller* poller = nfc_poller_alloc(app->nfc, NfcProtocolMfUltralight);
        nfc_poller_start(poller, poller_callback, app);

        uint32_t flags = furi_event_flag_wait(
            app->events, READ_DONE_FLAG, FuriFlagWaitAny, 200);

        nfc_poller_stop(poller);
        nfc_poller_free(poller);

        if((flags & FuriFlagError) || !app->card_present) break;
        furi_delay_ms(100);
    }

    app->detect_only = false;
}

// ---------------------------------------------------------------- Scan worker
static int32_t scan_worker(void* ctx) {
    App* app = ctx;

    while(app->running) {
        if(!app->scanning) {
            furi_delay_ms(50);
            continue;
        }

        scan_view_set_state(app, ScanStateWaiting);
        tag_data.ok = false;
        tag_data.pages_read = 0;
        app->status[0] = '\0';

        furi_event_flag_clear(
            app->events,
            READ_DONE_FLAG | READ_FAIL_FLAG | OVERWRITE_YES_FLAG | OVERWRITE_NO_FLAG |
                CONFIRM_YES_FLAG | CONFIRM_NO_FLAG | CANCEL_FLAG);

        NfcPoller* poller = nfc_poller_alloc(app->nfc, NfcProtocolMfUltralight);
        nfc_poller_start(poller, poller_callback, app);

        uint32_t flags = furi_event_flag_wait(
            app->events,
            READ_DONE_FLAG | READ_FAIL_FLAG | CANCEL_FLAG,
            FuriFlagWaitAny,
            FuriWaitForever);
        nfc_poller_stop(poller);
        nfc_poller_free(poller);

        if(!app->running) break;
        if(flags & CANCEL_FLAG) continue;
        if((flags & FuriFlagError) || (flags & READ_FAIL_FLAG) || !tag_data.ok) {
            strlcpy(app->status, "card read failed", sizeof(app->status));
            scan_view_set_state(app, ScanStateFailed);
            wait_for_card_removal(app);
            continue;
        }

        if(tag_data.pages_read < TOTAL_PAGES - 5) {
            snprintf(app->status, sizeof(app->status),
                     "only %zu pages (not NTAG216?)", tag_data.pages_read);
            scan_view_set_state(app, ScanStateFailed);
            wait_for_card_removal(app);
            continue;
        }

        if(!extract_name(tag_data.pages, app->name, sizeof(app->name))) {
            strlcpy(app->status, "no name found on card", sizeof(app->status));
            scan_view_set_state(app, ScanStateFailed);
            wait_for_card_removal(app);
            continue;
        }

        char safe[64];
        sanitize(app->name, safe, sizeof(safe));
        if(safe[0] == '\0') strlcpy(safe, "unnamed", sizeof(safe));
        strlcpy(app->name, safe, sizeof(app->name));
        snprintf(app->filename, sizeof(app->filename), "%s/%s.nfc", FOLDER, safe);

        vibro_pulse(80);

        if(app->mode_slow) {
            scan_view_set_state(app, ScanStateConfirmSave);
            uint32_t answer = furi_event_flag_wait(
                app->events,
                CONFIRM_YES_FLAG | CONFIRM_NO_FLAG | CANCEL_FLAG,
                FuriFlagWaitAny,
                FuriWaitForever);
            if(!app->running) break;
            if((answer & CANCEL_FLAG) || !app->scanning) continue;
            if(answer & CONFIRM_NO_FLAG) {
                wait_for_card_removal(app);
                continue;
            }
        }

        Storage* storage = furi_record_open(RECORD_STORAGE);
        bool exists = storage_file_exists(storage, app->filename);
        furi_record_close(RECORD_STORAGE);

        if(exists) {
            scan_view_set_state(app, ScanStateAskOverwrite);
            uint32_t answer = furi_event_flag_wait(
                app->events,
                OVERWRITE_YES_FLAG | OVERWRITE_NO_FLAG | CANCEL_FLAG,
                FuriFlagWaitAny,
                FuriWaitForever);
            if(!app->running) break;
            if((answer & CANCEL_FLAG) || !app->scanning) continue;
            if(answer & OVERWRITE_NO_FLAG) {
                wait_for_card_removal(app);
                continue;
            }
        }

        scan_view_set_state(app, ScanStateSaving);

        storage = furi_record_open(RECORD_STORAGE);
        storage_common_mkdir(storage, FOLDER);
        furi_record_close(RECORD_STORAGE);

        if(write_nfc_file(app->filename, tag_data.uid, tag_data.signature, tag_data.pages)) {
            scan_view_set_state(app, ScanStateDone);
        } else {
            snprintf(app->status, sizeof(app->status), "write error: %.100s", app->filename);
            scan_view_set_state(app, ScanStateFailed);
            furi_delay_ms(1000);
        }

        wait_for_card_removal(app);
    }

    return 0;
}
#endif // ENABLE_WORKER

// ---------------------------------------------------------------- Menu callbacks
static void submenu_cb(void* ctx, uint32_t index) {
    App* app = ctx;
    vibro_pulse(30);

    switch(index) {
    case 0: // Read Card
        app->scanning = true;
        app->current_view = ViewScan;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewScan);
        break;
    case 1: // Playback
        app->current_view = ViewPlayback;
        app->file_count = 0;
        app->file_index = 0;
        playback_view_set(app, "", 0, 0, false, true);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewPlayback);
        break;
    case 2: // Settings
        app->current_view = ViewSettings;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSettings);
        break;
    case 3: // About
        app->current_view = ViewAbout;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewAbout);
        break;
    default:
        break;
    }
}

static void slow_mode_change_cb(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    app->mode_slow = (index == 1);
    variable_item_set_current_value_text(item, app->mode_slow ? "ON" : "OFF");
    vibro_pulse(30);
}

// ---------------------------------------------------------------- Main
int32_t creature_scanner_app(void* p) {
    UNUSED(p);
    App app = {0};
    app.running = true;
    app.scanning = false;
    app.current_view = ViewSubmenu;
    app.mode_slow = false;
    app.detect_only = false;
    app.emulating = false;
    app.listener = NULL;
    app.emu_device = NULL;
    app.file_count = 0;
    app.file_index = 0;
    app.status[0] = '\0';

    app.files = malloc(MAX_FILES * sizeof(*app.files));

    app.gui = furi_record_open(RECORD_GUI);
    app.view_dispatcher = view_dispatcher_alloc();

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    view_dispatcher_enable_queue(app.view_dispatcher);
#pragma GCC diagnostic pop

    view_dispatcher_attach_to_gui(app.view_dispatcher, app.gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app.view_dispatcher, &app);
    view_dispatcher_set_navigation_event_callback(app.view_dispatcher, app_navigation_cb);

    app.submenu = submenu_alloc();
    submenu_add_item(app.submenu, "Read Card", 0, submenu_cb, &app);
    submenu_add_item(app.submenu, "Playback", 1, submenu_cb, &app);
    submenu_add_item(app.submenu, "Settings", 2, submenu_cb, &app);
    submenu_add_item(app.submenu, "About", 3, submenu_cb, &app);
    view_dispatcher_add_view(app.view_dispatcher, ViewSubmenu, submenu_get_view(app.submenu));

    app.variable_item_list = variable_item_list_alloc();
    VariableItem* item = variable_item_list_add(
        app.variable_item_list, "Slow mode", 2, slow_mode_change_cb, &app);
    variable_item_set_current_value_index(item, 0);
    variable_item_set_current_value_text(item, "OFF");
    view_dispatcher_add_view(
        app.view_dispatcher, ViewSettings,
        variable_item_list_get_view(app.variable_item_list));

    app.widget_about = widget_alloc();
    widget_add_text_scroll_element(
        app.widget_about,
        0,
        16,
        128,
        48,
        "Creature Scanner v1.0\nScans NTAG216 creature cards and saves them to the "
        "SD card, auto-named from the data on the card.\nSaves to:\n/ext/nfc/creatures\n"
        "Playback emulates saved cards.");
    view_dispatcher_add_view(
        app.view_dispatcher, ViewAbout, widget_get_view(app.widget_about));

    app.view_scan = view_alloc();
    view_allocate_model(app.view_scan, ViewModelTypeLocking, sizeof(ScanModel));
    view_set_draw_callback(app.view_scan, scan_view_draw_cb);
    view_set_context(app.view_scan, &app);
    view_set_input_callback(app.view_scan, scan_view_input_cb);
    view_dispatcher_add_view(app.view_dispatcher, ViewScan, app.view_scan);

    app.view_playback = view_alloc();
    view_allocate_model(app.view_playback, ViewModelTypeLocking, sizeof(PlaybackModel));
    view_set_draw_callback(app.view_playback, playback_view_draw_cb);
    view_set_context(app.view_playback, &app);
    view_set_input_callback(app.view_playback, playback_view_input_cb);
    view_dispatcher_add_view(app.view_dispatcher, ViewPlayback, app.view_playback);

    app.nfc = nfc_alloc();
    app.events = furi_event_flag_alloc();

#if ENABLE_WORKER
    app.worker = furi_thread_alloc_ex("CreatureScanWorker", 3 * 1024, scan_worker, &app);
    furi_thread_start(app.worker);
#else
    app.worker = NULL;
#endif

    app.playback_worker =
        furi_thread_alloc_ex("CreaturePlaybackWorker", 4 * 1024, playback_worker, &app);
    furi_thread_start(app.playback_worker);

    view_dispatcher_switch_to_view(app.view_dispatcher, ViewSubmenu);
    view_dispatcher_run(app.view_dispatcher);

    app.running = false;
    app.scanning = false;
    furi_event_flag_set(
        app.events, CANCEL_FLAG | PB_EXIT_FLAG | PB_NEXT_FLAG | PB_DEL_YES_FLAG);
#if ENABLE_WORKER
    furi_thread_join(app.worker);
    furi_thread_free(app.worker);
#endif
    furi_thread_join(app.playback_worker);
    furi_thread_free(app.playback_worker);

    playback_stop_emulation(&app);
    view_dispatcher_remove_view(app.view_dispatcher, ViewPlayback);
    view_dispatcher_remove_view(app.view_dispatcher, ViewScan);
    view_dispatcher_remove_view(app.view_dispatcher, ViewAbout);
    view_dispatcher_remove_view(app.view_dispatcher, ViewSettings);
    view_dispatcher_remove_view(app.view_dispatcher, ViewSubmenu);
    view_free(app.view_playback);
    view_free(app.view_scan);
    widget_free(app.widget_about);
    variable_item_list_free(app.variable_item_list);
    submenu_free(app.submenu);
    view_dispatcher_free(app.view_dispatcher);
    furi_event_flag_free(app.events);
    nfc_free(app.nfc);
    free(app.files);
    furi_record_close(RECORD_GUI);
    return 0;
}