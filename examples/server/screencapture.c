/*
 * screencapture.c - Simple VNC server that captures the X11 screen and
 *                   forwards mouse and keyboard events back to the display.
 *
 * Requires: libvncserver, libxcb, libxcb-xtest, libxcb-keysyms
 *
 * Build (manual):
 *   gcc screencapture.c -o screencapture \
 *       -lvncserver -lxcb -lxcb-xtest -lxcb-keysyms
 *
 * Build (CMake): included automatically when XCB libraries are found.
 *
 * Usage:
 *   ./screencapture [-rfbport <port>]
 *   Then connect any VNC viewer to localhost:<port> (default 5900).
 *
 * Notes:
 *   - Wayland does not allow direct screen reading via XCB.  Run under an
 *     X11 session (or inside XWayland only for the capture window).
 *   - The server captures the full root window at ~15 fps and only sends
 *     rows that have actually changed to minimise bandwidth.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <rfb/rfb.h>

#include <xcb/xcb.h>
#include <xcb/xtest.h>
#include <xcb/xcb_keysyms.h>

/* -------------------------------------------------------------------------
 * Target capture frame rate.
 * ------------------------------------------------------------------------- */
#define TARGET_FPS 15

/* -------------------------------------------------------------------------
 * Global XCB connection used by the input-forwarding callbacks.
 * Using a global avoids the need to pass it through rfbClientPtr->clientData
 * just for this straightforward example.
 * ------------------------------------------------------------------------- */
static xcb_connection_t *g_conn = NULL;

/* =========================================================================
 * Screen capture helpers
 * ========================================================================= */

/**
 * get_root_size - Query the dimensions of the root (desktop) window.
 */
static void get_root_size(xcb_connection_t *conn,
                          xcb_window_t       root,
                          uint16_t          *out_width,
                          uint16_t          *out_height)
{
    xcb_get_geometry_cookie_t cookie = xcb_get_geometry(conn, root);
    xcb_get_geometry_reply_t *reply  = xcb_get_geometry_reply(conn, cookie, NULL);
    if (!reply) {
        fprintf(stderr, "screencapture: xcb_get_geometry failed\n");
        exit(EXIT_FAILURE);
    }
    *out_width  = reply->width;
    *out_height = reply->height;
    free(reply);
}

/**
 * capture_screen - Capture the root window pixels into buf[].
 *
 * The X server returns pixels in BGRX order (32 bpp); we convert in-place
 * to RGBX so that libvncserver (which defaults to RGB) sees the right colours.
 */
static void capture_screen(xcb_connection_t *conn,
                            xcb_window_t      root,
                            uint8_t          *buf,
                            uint16_t          width,
                            uint16_t          height)
{
    xcb_get_image_cookie_t cookie =
        xcb_get_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
                      root, 0, 0, width, height, UINT32_MAX);
    xcb_get_image_reply_t *reply = xcb_get_image_reply(conn, cookie, NULL);
    if (!reply) {
        /* This is expected on Wayland; just skip the frame. */
        return;
    }

    const uint8_t *src = xcb_get_image_data(reply);
    uint32_t       npixels = (uint32_t)width * height;

    /* Convert BGRX -> RGBX */
    for (uint32_t i = 0; i < npixels; i++) {
        buf[i * 4 + 0] = src[i * 4 + 2]; /* R <- B */
        buf[i * 4 + 1] = src[i * 4 + 1]; /* G <- G */
        buf[i * 4 + 2] = src[i * 4 + 0]; /* B <- R */
        buf[i * 4 + 3] = 0;              /* X (unused alpha) */
    }

    free(reply);
}

/**
 * update_framebuffer - Copy changed rows from src into the libvncserver
 * framebuffer and mark them dirty.  Only rows that differ are sent, which
 * greatly reduces network traffic for static screens.
 */
static void update_framebuffer(rfbScreenInfoPtr screen,
                               const uint8_t   *src,
                               int              width,
                               int              height)
{
    const int bpp = 4;

    for (int y = 0; y < height; y++) {
        const uint8_t *src_row = src  + (size_t)y * width * bpp;
        uint8_t       *dst_row = (uint8_t *)screen->frameBuffer
                                 + (size_t)y * width * bpp;

        if (memcmp(dst_row, src_row, (size_t)width * bpp) != 0) {
            memcpy(dst_row, src_row, (size_t)width * bpp);
            rfbMarkRectAsModified(screen, 0, y, width, y + 1);
        }
    }
}

/* =========================================================================
 * Input-forwarding helpers (keyboard & mouse -> XTest)
 * ========================================================================= */

static void send_key(xcb_connection_t *conn, xcb_keysym_t keysym, int press)
{
    xcb_key_symbols_t *syms = xcb_key_symbols_alloc(conn);
    xcb_keycode_t     *codes = xcb_key_symbols_get_keycode(syms, keysym);

    if (codes) {
        for (xcb_keycode_t *kc = codes; *kc != XCB_NO_SYMBOL; kc++) {
            xcb_test_fake_input(conn,
                                press ? XCB_KEY_PRESS : XCB_KEY_RELEASE,
                                *kc,
                                XCB_CURRENT_TIME,
                                XCB_NONE, 0, 0, 0);
        }
        free(codes);
    }

    xcb_key_symbols_free(syms);
    xcb_flush(conn);
}

static void send_button(xcb_connection_t *conn, xcb_button_t button, int press)
{
    xcb_test_fake_input(conn,
                        press ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE,
                        button,
                        XCB_CURRENT_TIME,
                        XCB_NONE, 0, 0, 0);
    xcb_flush(conn);
}

static void send_motion(xcb_connection_t *conn, int16_t x, int16_t y)
{
    xcb_test_fake_input(conn,
                        XCB_MOTION_NOTIFY,
                        0,
                        XCB_CURRENT_TIME,
                        XCB_NONE, x, y, 0);
    xcb_flush(conn);
}

/* =========================================================================
 * libvncserver callbacks
 * ========================================================================= */

static void keyboard_callback(rfbBool down, rfbKeySym keysym,
                               rfbClientPtr client)
{
    (void)client;
    send_key(g_conn, (xcb_keysym_t)keysym, (int)down);
}

static void pointer_callback(int buttonMask, int x, int y,
                              rfbClientPtr client)
{
    (void)client;

    /* Forward each button individually; XTest needs a separate event per
     * button state change. */
    send_button(g_conn, XCB_BUTTON_INDEX_1,
                !!(buttonMask & rfbButton1Mask));
    send_button(g_conn, XCB_BUTTON_INDEX_2,
                !!(buttonMask & rfbButton2Mask));
    send_button(g_conn, XCB_BUTTON_INDEX_3,
                !!(buttonMask & rfbButton3Mask));
    /* Scroll wheel: button 4 = up, 5 = down */
    send_button(g_conn, XCB_BUTTON_INDEX_4,
                !!(buttonMask & rfbWheelUpMask));
    send_button(g_conn, XCB_BUTTON_INDEX_5,
                !!(buttonMask & rfbWheelDownMask));

    send_motion(g_conn, (int16_t)x, (int16_t)y);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    /* --- Connect to the X display --- */
    g_conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(g_conn)) {
        fprintf(stderr, "screencapture: cannot connect to X display\n");
        return EXIT_FAILURE;
    }

    const xcb_setup_t      *setup  = xcb_get_setup(g_conn);
    xcb_screen_iterator_t   iter   = xcb_setup_roots_iterator(setup);
    xcb_screen_t           *screen = iter.data;
    xcb_window_t            root   = screen->root;

    /* Query the current desktop size. */
    uint16_t width = 0, height = 0;
    get_root_size(g_conn, root, &width, &height);

    /* Scratch buffer for the raw pixel grab (RGBX after conversion). */
    uint8_t *capture_buf = malloc(4UL * width * height);
    if (!capture_buf) {
        fprintf(stderr, "screencapture: out of memory\n");
        xcb_disconnect(g_conn);
        return EXIT_FAILURE;
    }

    /* --- Set up the VNC server --- */
    rfbScreenInfoPtr rfb = rfbGetScreen(&argc, argv,
                                        (int)width, (int)height,
                                        8, 3, 4);
    if (!rfb) {
        fprintf(stderr, "screencapture: rfbGetScreen failed\n");
        free(capture_buf);
        xcb_disconnect(g_conn);
        return EXIT_FAILURE;
    }

    rfb->desktopName   = "screencapture";
    rfb->frameBuffer   = malloc(4UL * width * height);
    rfb->alwaysShared  = TRUE;
    rfb->kbdAddEvent   = keyboard_callback;
    rfb->ptrAddEvent   = pointer_callback;

    if (!rfb->frameBuffer) {
        fprintf(stderr, "screencapture: out of memory for framebuffer\n");
        rfbScreenCleanup(rfb);
        free(capture_buf);
        xcb_disconnect(g_conn);
        return EXIT_FAILURE;
    }

    /* Take an initial screenshot so the framebuffer is not empty when the
     * first client connects. */
    capture_screen(g_conn, root, capture_buf, width, height);
    memcpy(rfb->frameBuffer, capture_buf, 4UL * width * height);

    /* Start the VNC event loop in a background thread. */
    rfbInitServer(rfb);
    rfbRunEventLoop(rfb, -1 /* non-blocking poll */, TRUE /* threaded */);

    fprintf(stderr,
            "screencapture: listening on port %d (press Ctrl-C to quit)\n",
            rfb->port);

    /* --- Main capture loop --- */
    const long frame_ns = 1000000000L / TARGET_FPS;
    struct timespec ts_sleep = { 0, frame_ns };

    while (rfbIsActive(rfb)) {
        capture_screen(g_conn, root, capture_buf, width, height);
        update_framebuffer(rfb, capture_buf, (int)width, (int)height);
        nanosleep(&ts_sleep, NULL);
    }

    /* --- Clean up --- */
    free(rfb->frameBuffer);
    rfbScreenCleanup(rfb);
    free(capture_buf);
    xcb_disconnect(g_conn);

    return EXIT_SUCCESS;
}
