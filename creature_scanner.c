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
#include <nfc/nfc_poller.h>
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

#define READ_DONE_FLAG     (1u << 0)
#define READ_FAIL_FLAG     (1u << 1)
#define OVERWRITE_YES_FLAG (1u << 2)
#define OVERWRITE_NO_FLAG  (1u << 3)
#define CONFIRM_YES_FLAG   (1u << 4)
#define CONFIRM_NO_FLAG    (1u << 5)
#define CANCEL_FLAG        (1u << 6)

typedef enum {
    ScanStateWaiting,
    ScanStateReading,
    ScanStateSaving,
    ScanStateDone,
    ScanStateAskOverwrite,
    ScanStateConfirmSave,
    ScanStateFailed
} ScanState;

typedef enum { ViewSubmenu, ViewSettings, ViewAbout, ViewScan } AppView;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    VariableItemList* variable_item_list;
    Widget* widget_about;
    View* view_scan;
    Nfc* nfc;
    FuriEventFlag* events;
    FuriThread* worker;
    bool running;
    bool scanning;      // true while the Read view is active
    AppView current_view; // which view is on screen (for Back handling)
    bool mode_slow;     // slow mode: confirm each save
    bool detect_only;   // poller only detects card presence (removal check)
    bool card_present;  // last probe result (removal check)
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

    // Everything else (incl. Back) falls through to the navigation callback
    return false;
}

// Global navigation callback (Back key):
//   Settings/About -> Submenu
//   Read view      -> stop scanning, Submenu
//   Submenu        -> exit app (stop the dispatcher)
static bool app_navigation_cb(void* ctx) {
    App* app = ctx;

    if(app->current_view == ViewSettings || app->current_view == ViewAbout) {
        app->current_view = ViewSubmenu;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSubmenu);
        return true; // consumed, don't exit
    }

    if(app->current_view == ViewScan) {
        app->scanning = false;
        app->current_view = ViewSubmenu;
        furi_event_flag_set(app->events, CANCEL_FLAG);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSubmenu);
        return true; // consumed, don't exit
    }

    // Back on the home menu: stop the dispatcher -> view_dispatcher_run returns
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

    // Detection-only mode: used by wait_for_card_removal()
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

    // 7-byte UID: page 0 bytes 0-2, page 1 bytes 0-2, page 2 byte 0
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

        if((flags & FuriFlagError) || !app->card_present) break; // card removed
        furi_delay_ms(100); // card still present, probe again
    }

    app->detect_only = false;
}

// ---------------------------------------------------------------- Scan worker
// Runs in its own thread for the whole app lifetime. It idles while the
// Read view is not active, and can be paused and resumed any number of times.
static int32_t scan_worker(void* ctx) {
    App* app = ctx;

    while(app->running) {
        // Idle while not scanning (home menu, settings, about)
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

        // Poll for NTAG/Ultralight tags — waits until a card appears
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
        if(flags & CANCEL_FLAG) continue;   // user left the Read view
        if((flags & FuriFlagError) || (flags & READ_FAIL_FLAG) || !tag_data.ok) {
            strlcpy(app->status, "card read failed", sizeof(app->status));
            scan_view_set_state(app, ScanStateFailed);
            wait_for_card_removal(app);
            continue;   // NOT break — pause/resume must work
        }

        if(tag_data.pages_read < TOTAL_PAGES - 5) {
            snprintf(app->status, sizeof(app->status),
                     "only %zu pages (not NTAG216?)", tag_data.pages_read);
            scan_view_set_state(app, ScanStateFailed);
            wait_for_card_removal(app);
            continue;
        }

        // Extract the name from the ASCII phrase at pages 207-212
        if(!extract_name(tag_data.pages, app->name, sizeof(app->name))) {
            strlcpy(app->status, "no name found on card", sizeof(app->status));
            scan_view_set_state(app, ScanStateFailed);
            wait_for_card_removal(app);
            continue;
        }

        // Sanitize and build the path
        char safe[64];
        sanitize(app->name, safe, sizeof(safe));
        if(safe[0] == '\0') strlcpy(safe, "unnamed", sizeof(safe));
        strlcpy(app->name, safe, sizeof(app->name));
        snprintf(app->filename, sizeof(app->filename), "%s/%s.nfc", FOLDER, safe);

        // Vibrate at detection (before any dialog / save)
        vibro_pulse(80);

        // Slow mode: ask the user to confirm every save
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
                continue;   // discarded, back to scanning
            }
        }

        // Ask for confirmation if the file already exists
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
                continue;   // skip this card
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

        // After a scan, wait until the card is removed before scanning again
        wait_for_card_removal(app);
        // Loop continues: pauses if scanning was turned off, else scans again
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
        app->scanning = true;   // worker picks this up and starts polling
        app->current_view = ViewScan;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewScan);
        break;
    case 1: // Settings
        app->current_view = ViewSettings;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSettings);
        break;
    case 2: // About
        app->current_view = ViewAbout;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewAbout);
        break;
    default:
        break;
    }
}

// VariableItemList: "Slow mode" item with OFF/ON values (like SubGhz "Bin RAW")
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
    app.status[0] = '\0';

    // GUI
    app.gui = furi_record_open(RECORD_GUI);
    app.view_dispatcher = view_dispatcher_alloc();

    // This SDK's ViewDispatcher does NOT auto-enable its event queue
    // (the function is only marked deprecated). Without it, view_dispatcher_run()
    // deadlocks on this firmware — so call it, suppressing the warning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    view_dispatcher_enable_queue(app.view_dispatcher);
#pragma GCC diagnostic pop

    // Fullscreen attach (official pattern; Desktop type deadlocks FAP loaders)
    view_dispatcher_attach_to_gui(app.view_dispatcher, app.gui, ViewDispatcherTypeFullscreen);
    // Wire the app pointer into all dispatcher callbacks
    view_dispatcher_set_event_callback_context(app.view_dispatcher, &app);
    view_dispatcher_set_navigation_event_callback(app.view_dispatcher, app_navigation_cb);

    // Submenu (home)
    app.submenu = submenu_alloc();
    submenu_add_item(app.submenu, "Read Card", 0, submenu_cb, &app);
    submenu_add_item(app.submenu, "Settings", 1, submenu_cb, &app);
    submenu_add_item(app.submenu, "About", 2, submenu_cb, &app);
    view_dispatcher_add_view(app.view_dispatcher, ViewSubmenu, submenu_get_view(app.submenu));

    // VariableItemList (settings)
    app.variable_item_list = variable_item_list_alloc();
    VariableItem* item = variable_item_list_add(
        app.variable_item_list, "Slow mode", 2, slow_mode_change_cb, &app);
    variable_item_set_current_value_index(item, 0);
    variable_item_set_current_value_text(item, "OFF");
    view_dispatcher_add_view(
        app.view_dispatcher, ViewSettings,
        variable_item_list_get_view(app.variable_item_list));

    // Widget (about)
    app.widget_about = widget_alloc();
    widget_add_text_scroll_element(
        app.widget_about,
        0,
        16,
        128,
        48,
        "Creature Scanner v1.0\n\nScans NTAG216 creature cards and saves them to the "
        "SD card, auto-named from the data on the card.\n\nSaves to:\n/ext/nfc/creatures");
    view_dispatcher_add_view(
        app.view_dispatcher, ViewAbout, widget_get_view(app.widget_about));

    // Custom scan view
    app.view_scan = view_alloc();
    view_allocate_model(app.view_scan, ViewModelTypeLocking, sizeof(ScanModel));
    view_set_draw_callback(app.view_scan, scan_view_draw_cb);
    view_set_context(app.view_scan, &app);
    view_set_input_callback(app.view_scan, scan_view_input_cb);
    view_dispatcher_add_view(app.view_dispatcher, ViewScan, app.view_scan);

    // NFC + events
    app.nfc = nfc_alloc();
    app.events = furi_event_flag_alloc();

#if ENABLE_WORKER
    // Worker thread (idles until Read Card is selected)
    app.worker = furi_thread_alloc_ex("CreatureScanWorker", 3 * 1024, scan_worker, &app);
    furi_thread_start(app.worker);
#else
    app.worker = NULL;
#endif

    // Run the UI until view_dispatcher_stop() is called (Back on the submenu)
    view_dispatcher_switch_to_view(app.view_dispatcher, ViewSubmenu);
    view_dispatcher_run(app.view_dispatcher);

    // Shut down the worker thread
    app.running = false;
    app.scanning = false;
    furi_event_flag_set(app.events, CANCEL_FLAG);
#if ENABLE_WORKER
    furi_thread_join(app.worker);
    furi_thread_free(app.worker);
#endif

    // Free everything
    view_dispatcher_remove_view(app.view_dispatcher, ViewScan);
    view_dispatcher_remove_view(app.view_dispatcher, ViewAbout);
    view_dispatcher_remove_view(app.view_dispatcher, ViewSettings);
    view_dispatcher_remove_view(app.view_dispatcher, ViewSubmenu);
    view_free(app.view_scan);
    widget_free(app.widget_about);
    variable_item_list_free(app.variable_item_list);
    submenu_free(app.submenu);
    view_dispatcher_free(app.view_dispatcher);
    furi_event_flag_free(app.events);
    nfc_free(app.nfc);
    furi_record_close(RECORD_GUI);
    return 0;
}