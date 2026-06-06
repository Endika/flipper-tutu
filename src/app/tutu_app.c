#include "../../include/app/tutu_app.h"
#include "../../include/data/levels.h"
#include "../../include/domain/board.h"
#include "../../include/persistence/progress.h"
#include "../../include/platform/storage_port.h"

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>

#define BX 2
#define BY 2
#define CELL 10

typedef enum { ScreenMenu, ScreenGame, ScreenWin } Screen;

typedef struct {
    Gui *gui;
    ViewPort *view_port;
    FuriMessageQueue *input_queue;
    FuriMutex *mutex;
    bool running;

    Screen screen;
    TutuProgress progress;

    uint16_t level_index; // current level (also menu cursor in Task 9)
    TutuBoard board;
    uint8_t selected; // highlighted piece index
    uint16_t moves;
    uint8_t blink; // frame counter for the highlight blink
} TutuApp;

// ---- game logic helpers ----

static void load_level(TutuApp *app, uint16_t index) {
    const TutuLevel *l = tutu_levels_get(index);
    tutu_board_init(&app->board, l->pieces, l->count);
    app->level_index = index;
    app->selected = TUTU_RED;
    app->moves = 0;
    app->screen = ScreenGame;
}

// ---- rendering ----

static void draw_cell_rect(Canvas *c, const TutuPiece *p, int *x, int *y, int *w, int *h) {
    *x = BX + p->c * CELL;
    *y = BY + p->r * CELL;
    *w = (p->o == TUTU_H ? p->len : 1) * CELL;
    *h = (p->o == TUTU_V ? p->len : 1) * CELL;
    UNUSED(c);
}

static void draw_game(Canvas *canvas, TutuApp *app) {
    // board frame
    canvas_draw_frame(canvas, BX - 1, BY - 1, TUTU_SIZE * CELL + 2, TUTU_SIZE * CELL + 2);
    // exit marker: arrow at right edge, exit row
    int ey = BY + TUTU_EXIT_ROW * CELL + CELL / 2;
    canvas_draw_line(canvas, BX + TUTU_SIZE * CELL, ey - 3, BX + TUTU_SIZE * CELL + 4, ey);
    canvas_draw_line(canvas, BX + TUTU_SIZE * CELL, ey + 3, BX + TUTU_SIZE * CELL + 4, ey);

    for (uint8_t i = 0; i < app->board.count; i++) {
        int x, y, w, h;
        draw_cell_rect(canvas, &app->board.pieces[i], &x, &y, &w, &h);
        if (i == TUTU_RED) {
            canvas_draw_rbox(canvas, x + 1, y + 1, w - 2, h - 2, 2); // solid = red car
        } else {
            canvas_draw_rframe(canvas, x + 1, y + 1, w - 2, h - 2, 2); // outline = others
        }
        if (i == app->selected && (app->blink & 4)) {
            canvas_draw_rframe(canvas, x, y, w, h, 3); // blinking selection border
        }
    }

    // HUD
    char buf[24];
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "Lvl %u", (unsigned)(app->level_index + 1));
    canvas_draw_str(canvas, 66, 12, buf);
    snprintf(buf, sizeof(buf), "Moves %u", (unsigned)app->moves);
    canvas_draw_str(canvas, 66, 26, buf);
    canvas_draw_str(canvas, 66, 52, "OK: next car");
    canvas_draw_str(canvas, 66, 62, "Hold OK: reset");
}

static void draw_win(Canvas *canvas, TutuApp *app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "Solved!");
    char buf[32];
    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "Level %u in %u moves", (unsigned)(app->level_index + 1),
             (unsigned)app->moves);
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, buf);
    canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignCenter, "OK: next  Back: menu");
}

static void draw_menu(Canvas *canvas, TutuApp *app); // defined in Task 9

static void render_cb(Canvas *canvas, void *ctx) {
    TutuApp *app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    switch (app->screen) {
        case ScreenMenu:
            draw_menu(canvas, app);
            break;
        case ScreenGame:
            draw_game(canvas, app);
            break;
        case ScreenWin:
            draw_win(canvas, app);
            break;
    }
    furi_mutex_release(app->mutex);
}

static void input_cb(InputEvent *event, void *ctx) {
    TutuApp *app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

// ---- input handling per screen ----

static void handle_game_input(TutuApp *app, InputEvent *e); // below
static void handle_menu_input(TutuApp *app, InputEvent *e); // Task 9
static void handle_win_input(TutuApp *app, InputEvent *e);  // Task 9

static void handle_game_input(TutuApp *app, InputEvent *e) {
    TutuPiece *sel = &app->board.pieces[app->selected];
    if (e->type == InputTypeShort || e->type == InputTypeRepeat) {
        switch (e->key) {
            case InputKeyOk:
                if (e->type == InputTypeShort)
                    app->selected = tutu_board_next_piece(&app->board, app->selected);
                break;
            case InputKeyLeft:
                if (sel->o == TUTU_H && tutu_board_move(&app->board, app->selected, -1))
                    app->moves++;
                break;
            case InputKeyRight:
                if (sel->o == TUTU_H && tutu_board_move(&app->board, app->selected, +1))
                    app->moves++;
                break;
            case InputKeyUp:
                if (sel->o == TUTU_V && tutu_board_move(&app->board, app->selected, -1))
                    app->moves++;
                break;
            case InputKeyDown:
                if (sel->o == TUTU_V && tutu_board_move(&app->board, app->selected, +1))
                    app->moves++;
                break;
            default:
                break;
        }
        if (tutu_board_won(&app->board)) {
            tutu_progress_complete_and_unlock(&app->progress, app->level_index,
                                              tutu_levels_count());
            tutu_storage_save_progress(&app->progress);
            app->screen = ScreenWin;
        }
    } else if (e->type == InputTypeLong) {
        if (e->key == InputKeyOk)
            load_level(app, app->level_index); // reset
        else if (e->key == InputKeyBack)
            app->screen = ScreenMenu; // Task 9 menu; reachable now as stub
    } else if (e->type == InputTypeShort && e->key == InputKeyBack) {
        app->screen = ScreenMenu;
    }
}

// ---- lifecycle ----

static TutuApp *app_alloc(void) {
    TutuApp *app = malloc(sizeof(TutuApp));
    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->running = true;
    app->blink = 0;

    if (!tutu_storage_load_progress(&app->progress))
        tutu_progress_default(&app->progress);

    view_port_draw_callback_set(app->view_port, render_cb, app);
    view_port_input_callback_set(app->view_port, input_cb, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);
    view_port_update(app->view_port); // force first draw — do NOT rely on the Apps-menu
                                      // loader animation (blank-UI-from-favourites bug)

    app->screen = ScreenMenu;
    app->level_index = app->progress.highest_unlocked;
    load_level(app, app->level_index);
    app->screen = ScreenMenu; // start on menu (Task 9); Task 8 stub menu jumps to game
    return app;
}

static void app_free(TutuApp *app) {
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t tutu_app_run(void) {
    TutuApp *app = app_alloc();
    InputEvent event;
    while (app->running) {
        if (furi_message_queue_get(app->input_queue, &event, 50) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            // global exit: long Back from menu
            if (app->screen == ScreenMenu && event.type == InputTypeShort &&
                event.key == InputKeyBack) {
                app->running = false;
            } else {
                switch (app->screen) {
                    case ScreenGame:
                        handle_game_input(app, &event);
                        break;
                    case ScreenMenu:
                        handle_menu_input(app, &event);
                        break;
                    case ScreenWin:
                        handle_win_input(app, &event);
                        break;
                }
            }
            furi_mutex_release(app->mutex);
        }
        app->blink++;
        view_port_update(app->view_port);
    }
    app_free(app);
    return 0;
}

// TEMP stubs — replaced in Task 9
static void draw_menu(Canvas *canvas, TutuApp *app) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 28, AlignCenter, AlignCenter, "TUTU");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 46, AlignCenter, AlignCenter, "OK: play  Back: exit");
    UNUSED(app);
}
static void handle_menu_input(TutuApp *app, InputEvent *e) {
    if (e->type == InputTypeShort && e->key == InputKeyOk)
        load_level(app, app->progress.highest_unlocked);
}
static void handle_win_input(TutuApp *app, InputEvent *e) {
    if (e->type == InputTypeShort && e->key == InputKeyOk) {
        uint16_t next = app->level_index + 1;
        if (next < tutu_levels_count())
            load_level(app, next);
        else
            app->screen = ScreenMenu;
    } else if (e->type == InputTypeShort && e->key == InputKeyBack) {
        app->screen = ScreenMenu;
    }
}
