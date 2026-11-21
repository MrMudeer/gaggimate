/**
 * @file      LV_Helper.cpp
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2024  Shenzhen Xin Yuan Electronic Technology Co.,
 * Ltd
 * @date      2024-01-22
 *
 */
#include "LV_Helper.h"
#include <algorithm>

#if LV_VERSION_CHECK(9, 0, 0)
#error "Currently not supported 9.x"
#endif

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static lv_color_t *buf = NULL;
static lv_color_t *buf1 = NULL;

// Software screen rotation for panels whose controller cannot swap rows/columns (e.g. the CO5300 on
// the VIEWE knob). Set with -DGM_DISPLAY_ROTATION=<0|1|2|3> (quarter turns clockwise). Only used in
// direct mode, where LVGL keeps the whole frame in one buffer we can read rotated.
#ifndef GM_DISPLAY_ROTATION
#define GM_DISPLAY_ROTATION 0
#endif
#if GM_DISPLAY_ROTATION != 0
static uint16_t *rotBuf = NULL;

// Logical (LVGL) pixel shown at panel pixel (px, py). Assumes a square panel (W == H).
static inline void panelToLogical(int32_t px, int32_t py, int32_t W, int32_t H, int32_t &x, int32_t &y) {
#if GM_DISPLAY_ROTATION == 1
    x = py;
    y = H - 1 - px;
#elif GM_DISPLAY_ROTATION == 2
    x = W - 1 - px;
    y = H - 1 - py;
#else
    x = W - 1 - py;
    y = px;
#endif
}
#endif

/* Display flushing */
static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
#if GM_DISPLAY_ROTATION != 0
    if (disp_drv->direct_mode && rotBuf) {
        const int32_t W = disp_drv->hor_res;
        const int32_t H = disp_drv->ver_res;
        // Panel rectangle covering the logical dirty area.
        int32_t c[4][2] = {{area->x1, area->y1}, {area->x2, area->y1}, {area->x1, area->y2}, {area->x2, area->y2}};
        int32_t px1 = W, py1 = H, px2 = -1, py2 = -1;
        for (auto &p : c) {
            int32_t px, py;
#if GM_DISPLAY_ROTATION == 1
            px = H - 1 - p[1];
            py = p[0];
#elif GM_DISPLAY_ROTATION == 2
            px = W - 1 - p[0];
            py = H - 1 - p[1];
#else
            px = p[1];
            py = W - 1 - p[0];
#endif
            px1 = std::min(px1, px);
            py1 = std::min(py1, py);
            px2 = std::max(px2, px);
            py2 = std::max(py2, py);
        }
        // AMOLED controllers want even-aligned windows; the full frame is in the buffer, so widening is free.
        px1 &= ~1;
        py1 &= ~1;
        px2 = std::min<int32_t>(W - 1, px2 | 1);
        py2 = std::min<int32_t>(H - 1, py2 | 1);
        const uint16_t *fb = reinterpret_cast<const uint16_t *>(color_p);
        uint16_t *dst = rotBuf;
        for (int32_t py = py1; py <= py2; py++) {
            for (int32_t px = px1; px <= px2; px++) {
                int32_t x, y;
                panelToLogical(px, py, W, H, x, y);
                *dst++ = fb[y * W + x];
            }
        }
        static_cast<Display *>(disp_drv->user_data)->pushColors(px1, py1, px2 - px1 + 1, py2 - py1 + 1, rotBuf);
        lv_disp_flush_ready(disp_drv);
        return;
    }
#endif
    static_cast<Display *>(disp_drv->user_data)
        ->pushColors(area->x1, area->y1, area->x2 + 1, area->y2 + 1, reinterpret_cast<uint16_t *>(color_p));
    lv_disp_flush_ready(disp_drv);
}

/*Read the touchpad*/
static void touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data) {
    static int16_t x, y;
    auto *board = static_cast<Display *>(indev_driver->user_data);
    uint8_t touched = board->getPoint(&x, &y, 1);
    if (touched) {
#if GM_DISPLAY_ROTATION != 0
        int32_t lx, ly;
        panelToLogical(x, y, board->width(), board->height(), lx, ly);
        x = lx;
        y = ly;
#endif
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PR;
        return;
    }
    data->state = LV_INDEV_STATE_REL;
}

#if LV_USE_LOG
void lv_log_print_g_cb(const char *buf) {
    Serial.println(buf);
    Serial.flush();
}
#endif

String lvgl_helper_get_fs_filename(String filename) {
    static String path;
    path = String("A") + ":" + (filename);
    return path;
}

const char *lvgl_helper_get_fs_filename(const char *filename) {
    static String path;
    path = String("A") + ":" + String(filename);
    return path.c_str();
}

void beginLvglHelper(Display &board, bool debug) {

    lv_init();

#if LV_USE_LOG
    if (debug) {
        lv_log_register_print_cb(lv_log_print_g_cb);
    }
#endif

    size_t lv_buffer_size = board.width() * board.height() * sizeof(lv_color_t);
    buf = static_cast<lv_color_t *>(ps_malloc(lv_buffer_size));
    assert(buf);

    if (!board.supportsDirectMode()) {
        buf1 = static_cast<lv_color_t *>(ps_malloc(lv_buffer_size));
        assert(buf1);
    }

    lv_disp_draw_buf_init(&draw_buf, buf, buf1, board.width() * board.height());

#if GM_DISPLAY_ROTATION != 0
    if (board.supportsDirectMode() && board.width() == board.height()) {
        rotBuf = static_cast<uint16_t *>(ps_malloc(board.width() * board.height() * sizeof(uint16_t)));
    }
#endif

    /*Initialize the display*/
    lv_disp_drv_init(&disp_drv);
    /* display resolution */
    disp_drv.hor_res = board.width();
    disp_drv.ver_res = board.height();
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 0;
    disp_drv.direct_mode = board.supportsDirectMode();
    disp_drv.user_data = &board;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    indev_drv.user_data = &board;
    lv_indev_drv_register(&indev_drv);
}
