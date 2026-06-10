/*
 * screencapture_win.c - Simple VNC server that captures the Windows desktop
 *                       and forwards mouse and keyboard events back to it.
 *
 * Requires: libvncserver, Windows GDI (gdi32), User32 (user32)
 *           Both are part of the standard Windows SDK — no extra libraries.
 *
 * Build (manual, MSVC):
 *   cl screencapture_win.c /I<path\to\libvncserver\include> \
 *      /link vncserver.lib user32.lib gdi32.lib
 *
 * Build (CMake): included automatically on WIN32 builds.
 *
 * Usage:
 *   screencapture_win.exe [-rfbport <port>]
 *   Then connect any VNC viewer to <host>:<port> (default 5900).
 *
 * Notes:
 *   - Captures the entire virtual desktop (all monitors) at ~15 fps.
 *   - Only rows that have actually changed are sent to connected clients.
 *   - Keyboard forwarding uses the RFB keysym-to-VK mapping; characters
 *     outside the ASCII range are injected via a Unicode scan code.
 *   - SetProcessDPIAware() is called so that on High-DPI systems the
 *     captured and injected coordinates match the physical screen pixels.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <rfb/rfb.h>
#include <rfb/keysym.h>

/* -------------------------------------------------------------------------
 * Target capture frame rate.
 * ------------------------------------------------------------------------- */
#define TARGET_FPS 15

/* =========================================================================
 * Screen capture
 * ========================================================================= */

/* Context kept alive for the lifetime of the server. */
static HDC     g_screen_dc  = NULL; /* DC for the whole virtual screen    */
static HDC     g_mem_dc     = NULL; /* Memory DC holding the captured bmp */
static HBITMAP g_bitmap     = NULL; /* DIB section with RGBX pixels       */
static uint8_t *g_bits      = NULL; /* Pointer into the DIB section data  */
static int      g_virt_x    = 0;    /* SM_XVIRTUALSCREEN origin           */
static int      g_virt_y    = 0;    /* SM_YVIRTUALSCREEN origin           */

/**
 * screen_init - Create GDI objects sized to the current virtual desktop.
 * Returns the width/height via out parameters.
 */
static BOOL screen_init(int *out_width, int *out_height)
{
    *out_width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *out_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_virt_x    = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_virt_y    = GetSystemMetrics(SM_YVIRTUALSCREEN);

    g_screen_dc = GetDC(NULL);
    if (!g_screen_dc) {
        fprintf(stderr, "screencapture_win: GetDC failed (%lu)\n",
                GetLastError());
        return FALSE;
    }

    g_mem_dc = CreateCompatibleDC(g_screen_dc);
    if (!g_mem_dc) {
        fprintf(stderr, "screencapture_win: CreateCompatibleDC failed\n");
        ReleaseDC(NULL, g_screen_dc);
        return FALSE;
    }

    /* Create a top-down DIB section so row 0 is the top of the screen.
     * We request 32 bpp RGBX so GDI gives us BGR0; we fix the byte order
     * in capture_screen(). */
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = *out_width;
    bmi.bmiHeader.biHeight      = -(*out_height); /* negative = top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_bitmap = CreateDIBSection(g_screen_dc, &bmi, DIB_RGB_COLORS,
                                (void **)&g_bits, NULL, 0);
    if (!g_bitmap || !g_bits) {
        fprintf(stderr, "screencapture_win: CreateDIBSection failed\n");
        DeleteDC(g_mem_dc);
        ReleaseDC(NULL, g_screen_dc);
        return FALSE;
    }

    SelectObject(g_mem_dc, g_bitmap);
    return TRUE;
}

static void screen_cleanup(void)
{
    if (g_bitmap)    DeleteObject(g_bitmap);
    if (g_mem_dc)    DeleteDC(g_mem_dc);
    if (g_screen_dc) ReleaseDC(NULL, g_screen_dc);
}

/**
 * capture_screen - BitBlt from the screen into g_bits[], then convert
 * GDI's BGR0 order to RGBX that libvncserver expects.
 */
static void capture_screen(uint8_t *dst, int width, int height)
{
    BitBlt(g_mem_dc, 0, 0, width, height,
           g_screen_dc, g_virt_x, g_virt_y, SRCCOPY | CAPTUREBLT);

    /* Convert BGR0 (g_bits) -> RGBX (dst) */
    const uint32_t npixels = (uint32_t)width * height;
    for (uint32_t i = 0; i < npixels; i++) {
        uint8_t b = g_bits[i * 4 + 0];
        uint8_t g = g_bits[i * 4 + 1];
        uint8_t r = g_bits[i * 4 + 2];
        dst[i * 4 + 0] = r;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = b;
        dst[i * 4 + 3] = 0;
    }
}

/**
 * update_framebuffer - Copy changed rows into the VNC framebuffer and mark
 * them dirty.  Unchanged rows are skipped to save bandwidth.
 */
static void update_framebuffer(rfbScreenInfoPtr screen,
                               const uint8_t   *src,
                               int              width,
                               int              height)
{
    const int bpp = 4;
    for (int y = 0; y < height; y++) {
        const uint8_t *src_row = src + (size_t)y * width * bpp;
        uint8_t       *dst_row = (uint8_t *)screen->frameBuffer
                                 + (size_t)y * width * bpp;
        if (memcmp(dst_row, src_row, (size_t)width * bpp) != 0) {
            memcpy(dst_row, src_row, (size_t)width * bpp);
            rfbMarkRectAsModified(screen, 0, y, width, y + 1);
        }
    }
}

/* =========================================================================
 * Keyboard forwarding — RFB keysym -> Win32 VK / scan code
 * ========================================================================= */

/**
 * keysym_to_vk - Map an RFB keysym to a Windows virtual-key code.
 * Returns 0 if the keysym cannot be handled via VK (use Unicode injection).
 */
static WORD keysym_to_vk(rfbKeySym keysym)
{
    /* Function keys */
    if (keysym >= XK_F1 && keysym <= XK_F12)
        return (WORD)(VK_F1 + (keysym - XK_F1));

    switch (keysym) {
    case XK_BackSpace:    return VK_BACK;
    case XK_Tab:          return VK_TAB;
    case XK_Return:       return VK_RETURN;
    case XK_Escape:       return VK_ESCAPE;
    case XK_Delete:       return VK_DELETE;
    case XK_Home:         return VK_HOME;
    case XK_End:          return VK_END;
    case XK_Page_Up:      return VK_PRIOR;
    case XK_Page_Down:    return VK_NEXT;
    case XK_Left:         return VK_LEFT;
    case XK_Right:        return VK_RIGHT;
    case XK_Up:           return VK_UP;
    case XK_Down:         return VK_DOWN;
    case XK_Insert:       return VK_INSERT;
    case XK_Shift_L:      return VK_LSHIFT;
    case XK_Shift_R:      return VK_RSHIFT;
    case XK_Control_L:    return VK_LCONTROL;
    case XK_Control_R:    return VK_RCONTROL;
    case XK_Alt_L:        return VK_LMENU;
    case XK_Alt_R:        return VK_RMENU;
    case XK_Super_L:      return VK_LWIN;
    case XK_Super_R:      return VK_RWIN;
    case XK_Caps_Lock:    return VK_CAPITAL;
    case XK_Num_Lock:     return VK_NUMLOCK;
    case XK_Scroll_Lock:  return VK_SCROLL;
    case XK_Print:        return VK_SNAPSHOT;
    case XK_Pause:        return VK_PAUSE;
    case XK_space:        return VK_SPACE;
    default:              break;
    }

    /* Printable ASCII range: map directly to VK */
    if (keysym >= 0x20 && keysym <= 0x7e) {
        /* VkKeyScanA converts a character to a VK code including shift state.
         * We only need the VK (low byte); the caller handles the shift key. */
        SHORT vk_shift = VkKeyScanA((CHAR)keysym);
        if (vk_shift != -1)
            return (WORD)(vk_shift & 0xFF);
    }

    return 0; /* Unknown — caller can try Unicode injection */
}

static void keyboard_callback(rfbBool down, rfbKeySym keysym,
                               rfbClientPtr client)
{
    (void)client;

    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));
    inp.type = INPUT_KEYBOARD;

    WORD vk = keysym_to_vk(keysym);
    if (vk != 0) {
        /* Known VK: use a scan code so modifier keys work correctly. */
        inp.ki.wVk      = vk;
        inp.ki.wScan    = (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
        inp.ki.dwFlags  = down ? 0 : KEYEVENTF_KEYUP;
    } else if (keysym <= 0xFFFF) {
        /* Unknown / non-ASCII: inject as Unicode character. */
        inp.ki.wScan   = (WORD)keysym;
        inp.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
    } else {
        return; /* keysym out of Unicode BMP range — skip */
    }

    SendInput(1, &inp, sizeof(INPUT));
}

/* =========================================================================
 * Mouse forwarding
 * ========================================================================= */

static void pointer_callback(int buttonMask, int x, int y,
                              rfbClientPtr client)
{
    int width  = client->screen->width;
    int height = client->screen->height;

    /* SendInput with MOUSEEVENTF_ABSOLUTE expects coordinates in the range
     * [0, 65535] normalised across the entire virtual desktop. */
    LONG abs_x = (LONG)((x * 65535L) / (width  - 1));
    LONG abs_y = (LONG)((y * 65535L) / (height - 1));

    /* Only emit button events when state has changed. */
    static int prev_mask = 0;
    int changed = buttonMask ^ prev_mask;
    prev_mask = buttonMask;

    /* --- Mouse move --- */
    INPUT inp;
    ZeroMemory(&inp, sizeof(inp));
    inp.type           = INPUT_MOUSE;
    inp.mi.dx          = abs_x;
    inp.mi.dy          = abs_y;
    inp.mi.dwFlags     = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE
                         | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &inp, sizeof(INPUT));

    /* --- Button events --- */
    struct {
        int     mask;
        DWORD   down_flag;
        DWORD   up_flag;
    } buttons[] = {
        { rfbButton1Mask,    MOUSEEVENTF_LEFTDOWN,   MOUSEEVENTF_LEFTUP   },
        { rfbButton2Mask,    MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP },
        { rfbButton3Mask,    MOUSEEVENTF_RIGHTDOWN,  MOUSEEVENTF_RIGHTUP  },
    };

    for (int i = 0; i < 3; i++) {
        if (changed & buttons[i].mask) {
            ZeroMemory(&inp, sizeof(inp));
            inp.type       = INPUT_MOUSE;
            inp.mi.dx      = abs_x;
            inp.mi.dy      = abs_y;
            inp.mi.dwFlags = (buttonMask & buttons[i].mask)
                             ? buttons[i].down_flag
                             : buttons[i].up_flag;
            inp.mi.dwFlags |= MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            SendInput(1, &inp, sizeof(INPUT));
        }
    }

    /* --- Scroll wheel (momentary) --- */
    if (buttonMask & rfbWheelUpMask) {
        ZeroMemory(&inp, sizeof(inp));
        inp.type          = INPUT_MOUSE;
        inp.mi.mouseData  = WHEEL_DELTA;
        inp.mi.dwFlags    = MOUSEEVENTF_WHEEL;
        SendInput(1, &inp, sizeof(INPUT));
    }
    if (buttonMask & rfbWheelDownMask) {
        ZeroMemory(&inp, sizeof(inp));
        inp.type          = INPUT_MOUSE;
        inp.mi.mouseData  = (DWORD)(-(int)WHEEL_DELTA);
        inp.mi.dwFlags    = MOUSEEVENTF_WHEEL;
        SendInput(1, &inp, sizeof(INPUT));
    }
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(int argc, char *argv[])
{
    /* On High-DPI systems, opt out of scaling so captured and injected
     * coordinates are in physical pixels, matching what clients see.
     * Available since Windows Vista; unconditional call is safe. */
    SetProcessDPIAware();

    /* --- Initialise GDI screen capture --- */
    int width = 0, height = 0;
    if (!screen_init(&width, &height)) {
        return EXIT_FAILURE;
    }

    /* Scratch buffer (RGBX) for the converted frame. */
    uint8_t *capture_buf = (uint8_t *)malloc(4UL * width * height);
    if (!capture_buf) {
        fprintf(stderr, "screencapture_win: out of memory\n");
        screen_cleanup();
        return EXIT_FAILURE;
    }

    /* --- Set up the VNC server --- */
    rfbScreenInfoPtr rfb = rfbGetScreen(&argc, argv, width, height, 8, 3, 4);
    if (!rfb) {
        fprintf(stderr, "screencapture_win: rfbGetScreen failed\n");
        free(capture_buf);
        screen_cleanup();
        return EXIT_FAILURE;
    }

    rfb->desktopName  = "screencapture_win";
    rfb->frameBuffer  = (char *)malloc(4UL * width * height);
    rfb->alwaysShared = TRUE;
    rfb->kbdAddEvent  = keyboard_callback;
    rfb->ptrAddEvent  = pointer_callback;

    if (!rfb->frameBuffer) {
        fprintf(stderr, "screencapture_win: out of memory for framebuffer\n");
        rfbScreenCleanup(rfb);
        free(capture_buf);
        screen_cleanup();
        return EXIT_FAILURE;
    }

    /* Fill the framebuffer before the first client connects. */
    capture_screen(capture_buf, width, height);
    memcpy(rfb->frameBuffer, capture_buf, 4UL * width * height);

    /* Start the VNC event loop in a background thread. */
    rfbInitServer(rfb);
    rfbRunEventLoop(rfb, -1 /* non-blocking poll */, TRUE /* threaded */);

    fprintf(stderr,
            "screencapture_win: listening on port %d (press Ctrl-C to quit)\n",
            rfb->port);

    /* --- Main capture loop (~TARGET_FPS fps) --- */
    const DWORD frame_ms = 1000 / TARGET_FPS;

    while (rfbIsActive(rfb)) {
        capture_screen(capture_buf, width, height);
        update_framebuffer(rfb, capture_buf, width, height);
        Sleep(frame_ms);
    }

    /* --- Clean up --- */
    free(rfb->frameBuffer);
    rfbScreenCleanup(rfb);
    free(capture_buf);
    screen_cleanup();

    return EXIT_SUCCESS;
}
