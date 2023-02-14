#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <windows.h>
#include <winuser.h>
#include <wingdi.h>

#include <wand/magick_wand.h>

#include <libjj/utils.h>
#include <libjj/logging.h>
#include <libjj/jkey.h>
#include <libjj/iconv.h>
#include <libjj/config_opts.h>

#define DEFAULT_OUTPUT_FMT              "bmp"
#define DEFAULT_JSON_PATH               "config.json"
#define DEFAULT_WORK_PATH               "."
#define DEFAULT_BG_COLOR                "#000000"

#define MONITOR_COUNT_MAX               8

enum wallpaper_style {
        WALLPAPER_STYLE_FIT = 0,
        WALLPAPER_STYLE_FIT_EDGE_CUT,
        // WALLPAPER_STYLE_FIT_WIDTH,
        // WALLPAPER_STYLE_FIT_HEIGHT,
        WALLPAPER_STYLE_STRETCH,
        WALLPAPER_STYLE_TILE,
        WALLPAPER_STYLE_CENTER,
        NUM_WALLPAPER_STYLES,
};

// clockwise
enum monitor_orientation {
        ORIENT_0 = 0,            // landscape
        ORIENT_90,               // portrait
        ORIENT_180,              // landscape(flipped)
        ORIENT_270,              // portrait(flipped) in settings
        NUM_MONITOR_ORIENTS,
        ORIENT_UNKNOWN,
};

char *wallpaper_style_strs[] = {
        [WALLPAPER_STYLE_FIT]           = "fit_no_cut",
        [WALLPAPER_STYLE_FIT_EDGE_CUT]  = "fit_edge_cut",
        [WALLPAPER_STYLE_STRETCH]       = "stretch",
        [WALLPAPER_STYLE_TILE]          = "tile",
        [WALLPAPER_STYLE_CENTER]        = "center",
};

struct line {
        int32_t s, e;
};

struct rectangle {
        int32_t x;
        int32_t y;
        uint32_t width;
        uint32_t height;
};

struct monitor {
        uint8_t active;

        struct {
                int32_t         x;
                int32_t         y;
                uint32_t        width;
                uint32_t        height;
                uint32_t        orientation;
                uint8_t         is_primary;
        } info;

        struct {
                int32_t         x;
                int32_t         y;
        } virt_pos;

        struct {
                uint32_t        auto_rotate;
                int             style;
                char           *bg_color;
                char           *files[NUM_MONITOR_ORIENTS];
        } wallpaper;
};

// y axi of virtual desktop is inverted:
//      y
//      |
//  x---+----->
//      |
//      V
struct rectangle virtual_desktop;

struct config {
        char output_fmt[5];
        char workdir[PATH_MAX];
        char json_path[PATH_MAX];
};

static struct config g_config = {
        .json_path = DEFAULT_JSON_PATH,
};

static struct monitor monitors[MONITOR_COUNT_MAX];
static jbuf_t jbuf_usrcfg;
static char out_path[PATH_MAX] = {0 };
static wchar_t out_path_w[PATH_MAX] = { 0 };

opt_noarg('h', help, "This help message");
opt_strbuf('c', json_path, g_config.json_path, DEFAULT_JSON_PATH, "JSON config path");

static optdesc_t *opt_list[] = {
        &opt_help,
        &opt_json_path,
        NULL,
};

static uint32_t dmdo_to_orien[] = {
        [DMDO_DEFAULT]  = ORIENT_0,
        [DMDO_90]       = ORIENT_90,
        [DMDO_180]      = ORIENT_180,
        [DMDO_270]      = ORIENT_270,
};

static int usrcfg_root_key_create(jbuf_t *b)
{
        int err;
        void *root;

        if ((err = jbuf_init(b, JBUF_INIT_ALLOC_KEYS))) {
                pr_err("jbuf_init(), err = %d\n", err);
                return err;
        }

        root = jbuf_obj_open(b, NULL);

        {
                void *monitor_arr = jbuf_fixed_arr_open(b, "monitor");

                jbuf_fixed_arr_setup(b, monitor_arr,
                                     monitors,
                                     ARRAY_SIZE(monitors),
                                     sizeof(monitors[0]));
                void *monitor_obj = jbuf_offset_obj_open(b, NULL, 0);

                {
                        void *wallpaper_obj = jbuf_offset_obj_open(b, "wallpaper", 0);

                        {
                                jbuf_offset_add(b, bool, "auto_rotate", offsetof(struct monitor, wallpaper.auto_rotate));
                                jbuf_offset_strval_add(b, "style",
                                                       offsetof(struct monitor, wallpaper.style),
                                                       wallpaper_style_strs,
                                                       ARRAY_SIZE(wallpaper_style_strs));
                                jbuf_offset_add(b, strptr, "bg_color", offsetof(struct monitor, wallpaper.bg_color));

                                void *source_obj = jbuf_offset_obj_open(b, "source", 0);

                                jbuf_offset_add(b, strptr, "landscape_0", offsetof(struct monitor, wallpaper.files[ORIENT_0]));
                                jbuf_offset_add(b, strptr, "landscape_180", offsetof(struct monitor, wallpaper.files[ORIENT_180]));
                                jbuf_offset_add(b, strptr, "portrait_90", offsetof(struct monitor, wallpaper.files[ORIENT_90]));
                                jbuf_offset_add(b, strptr, "portrait_270", offsetof(struct monitor, wallpaper.files[ORIENT_270]));

                                jbuf_obj_close(b, source_obj);
                        }

                        jbuf_obj_close(b, wallpaper_obj);
                }

                jbuf_obj_close(b, monitor_obj);
                jbuf_arr_close(b, monitor_arr);

                void *settings_obj = jbuf_obj_open(b, "settings");

                {
                        jbuf_strbuf_add(b, "output_format", g_config.output_fmt, sizeof(g_config.output_fmt));
                        jbuf_strbuf_add(b, "workdir", g_config.workdir, sizeof(g_config.workdir));
                }

                jbuf_obj_close(b, settings_obj);
        }

        jbuf_obj_close(b, root);

        return 0;
}

static int usrcfg_init(void)
{
        jbuf_t *jbuf = &jbuf_usrcfg;
        int err;

        if ((err = usrcfg_root_key_create(jbuf)))
                return err;

        pr_info("json config: %s\n", g_config.json_path);

        if ((err = jbuf_load(jbuf, g_config.json_path)))
                return err;

        pr_info("json config loaded:\n");
        jbuf_traverse_print(jbuf);

        return 0;
}

static int usrcfg_deinit(void)
{
        return jbuf_deinit(&jbuf_usrcfg);
}

static int desktop_wallpaper_get(wchar_t *path, size_t len)
{
        wchar_t current[PATH_MAX] = { 0 };

        if (0 == SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, current, 0)) {
                pr_err("SystemParametersInfo() failed\n");
                return -EFAULT;
        }

        pr_info("current wallpaper path: %ls\n", current);

        if (path && len)
                wcsncpy(path, current, len);

        return 0;
}

static int desktop_wallpaper_set(wchar_t *file)
{
        wchar_t fullpath[MAX_PATH] = { 0 };

        pr_info("path: %ls\n", file);

        if (0 == _wfullpath(fullpath, file, MAX_PATH)) {
                pr_err("invalid path: %ls\n", file);
                return -EINVAL;
        }

        if (0 == SystemParametersInfo(SPI_SETDESKWALLPAPER, 0, fullpath, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
                pr_err("SystemParametersInfo() failed\n");
                return -EFAULT;
        }

        return 0;
}

static int __display_info_update(uint32_t idx, DISPLAY_DEVICE *dev, DEVMODE *mode)
{
        struct monitor *m = &monitors[idx];

        if (idx >= ARRAY_SIZE(monitors)) {
                pr_err("index is over monitor limit\n");
                return -E2BIG;
        }

        m->active = 0;

        if (!dev)
                return 0;

        if ((dev->StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) {
                m->active = 0;
        } else if (dev->StateFlags & DISPLAY_DEVICE_ACTIVE) {
                m->active = 1;
        }

        if (!m->active) {
                memset(&m->info, 0, sizeof(m->info));
                return 0;
        }

        m->info.x = mode->dmPosition.x;
        m->info.y = mode->dmPosition.y;
        m->info.width = mode->dmPelsWidth;
        m->info.height = mode->dmPelsHeight;
        m->info.orientation = mode->dmDisplayOrientation < ARRAY_SIZE(dmdo_to_orien) ?
                              dmdo_to_orien[mode->dmDisplayOrientation] : ORIENT_UNKNOWN;

        if (m->info.x == 0 && m->info.y == 0)
                m->info.is_primary = 1;
        else
                m->info.is_primary = 0;

        return 0;
}

static void display_info_update(void)
{
        for (DWORD i = 0; ; i++) {
                DISPLAY_DEVICE dev = { .cb = sizeof(DISPLAY_DEVICE) };
                DEVMODE mode = { .dmSize = sizeof(DEVMODE) };

                if (0 == EnumDisplayDevices(NULL, i, &dev, 0)) {
                        break;
                }

                if (0 == (dev.StateFlags & DISPLAY_DEVICE_ACTIVE)) {
                        pr_raw("Display #%lu (not active)\n", i);
                        goto update;
                }

                if ((dev.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)) {
                        pr_raw("Display #%lu (mirroring)\n", i);
                        goto update;
                }

                pr_raw("Display #%lu\n", i);
                pr_raw("       Name:   %ls\n", dev.DeviceName);
                pr_raw("       String: %ls\n", dev.DeviceString);
                pr_raw("       Flags:  0x%08lx\n", dev.StateFlags);
                pr_raw("       RegKey: %ls\n", dev.DeviceKey);

                if (!EnumDisplaySettings(dev.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
                        pr_err("EnumDisplaySettings() failed\n");
                        continue;
                }

                //
                // the primary display is always located at 0,0
                //

                pr_raw("       Mode: %lux%lu @ %lu Hz %lu bpp\n", mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency, mode.dmBitsPerPel);
                pr_raw("       Orientation: %lu\n", mode.dmDisplayOrientation);
                pr_raw("       Desktop position: ( %ld, %ld )\n", mode.dmPosition.x, mode.dmPosition.y);

update:
                __display_info_update(i, &dev, &mode);
        }
}

static int is_axis_cover_point(struct line *l, int32_t p)
{
        if (l->s < l->e) {
                if (l->s <= p && p <= l->e)
                        return 1;
        } else {
                if (l->e <= p && p <= l->s)
                        return 1;
        }

        return 0;
}

static void virtual_desktop_size_compute(struct rectangle *desk, struct rectangle *append)
{
        struct line dw = { .s = desk->x,   .e = desk->x   + desk->width };
        struct line dh = { .s = desk->y,   .e = desk->y   + desk->height };
        struct line aw = { .s = append->x, .e = append->x + append->width };
        struct line ah = { .s = append->y, .e = append->y + append->height };
        int32_t delta;

        // is first monitor
        if (desk->width == 0 && desk->height == 0) {
                desk->x = append->x;
                desk->y = append->y;
                desk->width = append->width;
                desk->height = append->height;

                return;
        }

        delta = 0;

        if (is_axis_cover_point(&aw, desk->x)) {
                // is desktop width covered by whole line?
                if (is_axis_cover_point(&aw, desk->x + desk->width)) {
                        delta = desk->width;
                } else {
                        delta = abs(aw.e - desk->x);
                }
        } else if (is_axis_cover_point(&dw, append->x)) {
                if (is_axis_cover_point(&dw, append->x + append->width)) {
                        delta = append->width;
                } else {
                        delta = abs(dw.e - append->x);
                }
        }

        desk->width = append->width + desk->width - delta;

        // new rectangle point always sits at left-top
        if (append->x < desk->x) {
                desk->x = append->x;
        }

        delta = 0;

        if (is_axis_cover_point(&ah, desk->y)) {
                if (is_axis_cover_point(&ah, desk->y + desk->height)) {
                        delta = desk->height;
                } else {
                        delta = abs(ah.e - desk->y);
                }
        } else if (is_axis_cover_point(&dh, append->y)) {
                if (is_axis_cover_point(&dh, append->y + append->height)) {
                        delta = append->height;
                } else {
                        delta = abs(dh.e - append->y);
                }
        }

        desk->height = append->height + desk->height - delta;

        if (append->y < desk->y) {
                desk->y = append->y;
        }
}

static int virtual_desktop_update(struct rectangle *virtdesk)
{
        for (size_t i = 0; i < ARRAY_SIZE(monitors); i++) {
                struct monitor *m = &monitors[i];

                if (!m->active)
                        continue;

                virtual_desktop_size_compute(virtdesk,
                                             &(struct rectangle){
                                                     .x = m->info.x,
                                                     .y = m->info.y,
                                                     .width = m->info.width,
                                                     .height = m->info.height
                                             });
        }

        return 0;
}

static int virtual_desktop_position_reposition(struct rectangle *virtdesk)
{
        if (virtdesk->height == 0 || virtdesk->width == 0)
                return -EINVAL;

        for (size_t i = 0; i < ARRAY_SIZE(monitors); i++) {
                struct monitor *m = &monitors[i];

                if (!m->active)
                        continue;

                m->virt_pos.x = m->info.x - virtdesk->x;
                m->virt_pos.y = m->info.y - virtdesk->y;
        }

        virtdesk->x = virtdesk->y = 0;

        return 0;
}

static int wallpaper_style_fit_apply(struct monitor *m, MagickWand *w)
{
        MagickPassFail status = MagickPass;
        uint32_t pic_width, pic_height;
        uint32_t mon_width, mon_height;
        double scale, mon_aspect, pic_aspect;
        const int FIT_WIDTH = 0, FIT_HEIGHT = 1;
        int style;

        mon_width = m->info.width;
        mon_height = m->info.height;
        pic_width = MagickGetImageWidth(w);
        pic_height = MagickGetImageHeight(w);
        mon_aspect = (double)mon_width / mon_height;
        pic_aspect = (double)pic_width / pic_height;

        if (pic_aspect > mon_aspect) {
                style = FIT_WIDTH;
                scale = (double)pic_width / m->info.width;
        } else {
                style = FIT_HEIGHT;
                scale = (double)pic_height / m->info.height;
        }

        pr_info("fit %s\n", style == FIT_WIDTH ? "width" : "height");

        status = MagickScaleImage(w, (unsigned long)(pic_width / scale), (unsigned long)(pic_height / scale));
        if (status != MagickPass)
                return -EFAULT;

        if (style == FIT_WIDTH) {
                uint32_t scaled_height = MagickGetImageHeight(w);
                if (scaled_height < mon_height) {
                        status = MagickExtentImage(w, mon_width, mon_height, 0, (mon_height - scaled_height) / 2);
                        if (status != MagickPass)
                                return -EFAULT;
                }
        } else if (style == FIT_HEIGHT) {
                uint32_t scaled_width = MagickGetImageWidth(w);
                if (scaled_width < mon_width) {
                        status = MagickExtentImage(w, mon_width, mon_height, (mon_width - scaled_width) / 2, 0);
                        if (status != MagickPass)
                                return -EFAULT;
                }
        }

        return 0;
}

static int wallpaper_style_fit_edge_cut_apply(struct monitor *m, MagickWand *w)
{
        MagickPassFail status = MagickPass;
        uint32_t pic_width, pic_height;
        uint32_t mon_width, mon_height;
        double scale, mon_aspect, pic_aspect;
        const int FIT_WIDTH = 0, FIT_HEIGHT = 1;
        int style;

        mon_width = m->info.width;
        mon_height = m->info.height;
        pic_width = MagickGetImageWidth(w);
        pic_height = MagickGetImageHeight(w);
        mon_aspect = (double)mon_width / mon_height;
        pic_aspect = (double)pic_width / pic_height;

        if (pic_aspect > mon_aspect) {
                style = FIT_HEIGHT;
                scale = (double)pic_height / m->info.height;
        } else {
                style = FIT_WIDTH;
                scale = (double)pic_width / m->info.width;
        }

        pr_info("fit %s\n", style == FIT_WIDTH ? "width" : "height");

        status = MagickScaleImage(w, (unsigned long)(pic_width / scale), (unsigned long)(pic_height / scale));
        if (status != MagickPass)
                return -EFAULT;

        if (style == FIT_WIDTH) {
                status = MagickCropImage(w, mon_width, mon_height, 0, (MagickGetImageHeight(w) - mon_height) / 2);
        } else if (style == FIT_HEIGHT) {
                status = MagickCropImage(w, mon_width, mon_height, (MagickGetImageWidth(w) - mon_width) / 2, 0);
        }

        if (status != MagickPass)
                return -EFAULT;

        return 0;
}

static int wallpaper_style_stretch_apply(struct monitor *m, MagickWand *w)
{
        MagickPassFail status = MagickPass;

        status = MagickScaleImage(w, m->info.width, m->info.height);
        if (status != MagickPass)
                return -EFAULT;

        return 0;
}

static int wallpaper_style_tile_apply(struct monitor *m, MagickWand *w)
{
        MagickWand *orig;
        MagickPassFail status = MagickPass;
        uint32_t pic_width = MagickGetImageWidth(w);
        uint32_t pic_height = MagickGetImageHeight(w);
        uint32_t mon_width = m->info.width;
        uint32_t mon_height = m->info.height;
        uint32_t x = 0, y = 0, filled_height = 0;
        int err = 0;

        if (pic_width >= mon_width && pic_height >= mon_height) {
                status = MagickCropImage(w, m->info.width, m->info.height, 0, 0);
                if (status != MagickPass)
                        return -EFAULT;

                return 0;
        }

        orig = CloneMagickWand(w);
        status = MagickExtentImage(w, mon_width, mon_height, mon_width, mon_height); // blank image
        if (status != MagickPass)
                return -EFAULT;

        while (filled_height < mon_height) {
                status = MagickCompositeImage(w, orig, OverCompositeOp, x, y);
                if (status != MagickPass) {
                        err = -EFAULT;
                        goto out;
                }

                x += pic_width;

                if (x >= mon_width) {
                        x = 0;
                        y += pic_height;
                        filled_height += pic_height;
                }
        }

out:
        DestroyMagickWand(orig);

        return err;
}

static int wallpaper_style_center_apply(struct monitor *m, MagickWand *w)
{
        MagickPassFail status = MagickPass;
        uint32_t pic_width, pic_height;
        uint32_t mon_width, mon_height;
        long x, y;

        mon_width = m->info.width;
        mon_height = m->info.height;
        pic_width = MagickGetImageWidth(w);
        pic_height = MagickGetImageHeight(w);

        if (mon_width == pic_width && mon_height == pic_width)
                return 0;

        if (pic_width > mon_width && pic_height > mon_height) {
                x = (pic_width / 2) - (mon_width / 2);
                y = (pic_height / 2) - (mon_height / 2);
                status = MagickCropImage(w, mon_width, mon_height, x, y);
        } else {
                x = (mon_width / 2) - (pic_width / 2);
                y = (mon_height / 2) - (pic_height / 2);
                status = MagickExtentImage(w, mon_width, mon_height, x, y);
        }

        if (status != MagickPass)
                return -EFAULT;

        return 0;
}

/**
 * @return orientation available, <0 not found
 */
static int wallpaper_auto_rotate(struct monitor *m)
{
        int flip_orient = ((m->info.orientation * 90 + 180) % 360) / 90;

        if (flip_orient >= NUM_MONITOR_ORIENTS)
                return -1;

        // opposite orientation is preferred
        if (m->wallpaper.files[flip_orient])
                return flip_orient;

        for (size_t i = 0; i < ARRAY_SIZE(m->wallpaper.files); i++) {
                if (m->wallpaper.files[i])
                        return (int)i;
        }

        return -1;
}

static int wallpaper_load(struct monitor *m, MagickWand **out)
{
        MagickPassFail status = MagickPass;
        MagickWand *w = NULL;
        PixelWand *bg = NULL;
        uint32_t orient = m->info.orientation;
        int alter_orient = -1;
        char *wallpaper_path;
        int err = 0;

        if (!m->active)
                return -ENODATA;

        if (orient >= NUM_MONITOR_ORIENTS) {
                pr_err("unknown orientation: %u\n", orient);
                return -EINVAL;
        }

        w = NewMagickWand();
        wallpaper_path = m->wallpaper.files[orient];

        if (!wallpaper_path || wallpaper_path[0] == '\0') {
                if (!m->wallpaper.auto_rotate)
                        return -ENODATA;

                alter_orient = wallpaper_auto_rotate(m);
                if (alter_orient < 0)
                        return -ENODATA;

                wallpaper_path = m->wallpaper.files[alter_orient];
        }

        status = MagickReadImage(w, wallpaper_path);
        if (status != MagickPass) {
                pr_err("failed to open wallpaper file: %s\n", wallpaper_path);
                err = -EIO;
                goto out_err;
        }

        bg = NewPixelWand();

        PixelSetColor(bg, DEFAULT_BG_COLOR);

        if (m->wallpaper.bg_color && m->wallpaper.bg_color[0] != '\0') {
                status = PixelSetColor(bg, m->wallpaper.bg_color);
                if (status != MagickPass)
                        PixelSetColor(bg, DEFAULT_BG_COLOR);
        }

        status = MagickSetImageBackgroundColor(w, bg);
        if (status != MagickPass) {
                err = -EFAULT;
                goto out_err;
        }

        if (alter_orient >= 0) {
                int rotation = (360 - (alter_orient * 90 - orient * 90)) % 360;
                status = MagickRotateImage(w, bg, (double)rotation);
                if (status != MagickPass)
                        return -EFAULT;
        }

        switch (m->wallpaper.style) {
        case WALLPAPER_STYLE_FIT:
                err = wallpaper_style_fit_apply(m, w);
                break;

        case WALLPAPER_STYLE_FIT_EDGE_CUT:
                err = wallpaper_style_fit_edge_cut_apply(m, w);
                break;

        case WALLPAPER_STYLE_STRETCH:
                err = wallpaper_style_stretch_apply(m, w);
                break;

        case WALLPAPER_STYLE_TILE:
                err = wallpaper_style_tile_apply(m, w);
                break;

        case WALLPAPER_STYLE_CENTER:
                err = wallpaper_style_center_apply(m, w);
                break;

        default:
                pr_err("unknown wallpaper style\n");
                err = -EINVAL;
                goto out_err;
        }

        if (out)
                *out = w;

        if (bg)
                DestroyPixelWand(bg);

        return err;

out_err:
        if (bg)
                DestroyPixelWand(bg);

        DestroyMagickWand(w);

        return err;
}

static int wallpaper_generate(void)
{
        struct rectangle *virt_desk = &virtual_desktop;
        MagickWand *wallpapers[MONITOR_COUNT_MAX] = { 0 };
        MagickWand *canvas = NULL;
        PixelWand *canvas_bg = NewPixelWand();
        MagickPassFail status = MagickPass;
        int err = 0;

        for (size_t i = 0; i < ARRAY_SIZE(monitors); i++) {
                struct monitor *m = &monitors[i];

                if (!m->active)
                        continue;

                if ((err = wallpaper_load(m, &wallpapers[i]))) {
                        pr_err("failed to load wallpaper of monitor %zu\n", i);
                        continue;
                }

//                {
//                        char path[PATH_MAX] = { 0 };
//                        snprintf(path, sizeof(path), "R:/%zu.bmp", i);
//                        status = MagickWriteImage(wallpapers[i], path);
//                        if (status != MagickPass)
//                                pr_err("failed to write image %s\n", path);
//                }
        }

        canvas = NewMagickWand();
        status = MagickReadImage(canvas, "XC:"); // create a blank image
        if (status != MagickPass)
                goto out_free_canvas;

        PixelSetColor(canvas_bg, DEFAULT_BG_COLOR);
        MagickSetImageBackgroundColor(canvas, canvas_bg);

        status = MagickExtentImage(canvas, virt_desk->width, virt_desk->height, 0, 0);
        if (status != MagickPass)
                goto out_free_canvas;

        for (size_t i = 0; i < ARRAY_SIZE(monitors); i++) {
                struct monitor *m = &monitors[i];

                if (!m->active || !wallpapers[i])
                        continue;

                status = MagickCompositeImage(canvas, wallpapers[i], OverCompositeOp, m->virt_pos.x, m->virt_pos.y);
                if (status != MagickPass) {
                        pr_err("failed to composite %zu wallpaper into canvas\n", i);
                        goto out_free_canvas;
                }
        }

        status = MagickWriteImage(canvas, out_path);
        if (status != MagickPass) {
                pr_err("failed to save wallpaper image to %s\n", out_path);
                err = -EIO;
                goto out_free_canvas;
        }

out_free_canvas:
        DestroyMagickWand(canvas);

        for (size_t i = 0; i < ARRAY_SIZE(wallpapers); i++) {
                if (wallpapers[i])
                        DestroyMagickWand(wallpapers[i]);
        }

        DestroyPixelWand(canvas_bg);

        return err;
}

static int wallpaper_update(void)
{
        int err;

        display_info_update();
        virtual_desktop_update(&virtual_desktop);
        virtual_desktop_position_reposition(&virtual_desktop);

        if ((err = wallpaper_generate())) {
                pr_err("wallpaper_generate() failed\n");
                return err;
        }

        if ((err = desktop_wallpaper_set(out_path_w))) {
                pr_err("desktop_wallpaper_set() failed\n");
                return err;
        }

        return 0;
}

static LRESULT CALLBACK notify_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
        if (msg != WM_DISPLAYCHANGE)
                goto def_proc;

        pr_info("display mode changed\n");
        // pr_info("display changed: bit: %lld %ux%u\n", wparam, LOWORD(lparam), HIWORD(lparam));

        wallpaper_update();

        return TRUE;

def_proc:
        return DefWindowProc(hwnd, msg, wparam, lparam);
}

static HANDLE notify_wnd_create(void)
{
        HWND wnd = NULL;
        WNDCLASSEX wc = { 0 };

        wc.cbSize              = sizeof(WNDCLASSEX);
        wc.lpfnWndProc         = notify_wnd_proc;
        wc.hInstance           = GetModuleHandle(NULL);
        wc.lpszClassName       = L"NotifyWnd";
        if (!RegisterClassEx(&wc)) {
                return NULL;
        }

        wnd = CreateWindowEx(0, L"NotifyWnd",
                             NULL, 0, 0, 0, 0, 0, 0, 0,
                             GetModuleHandle(NULL), 0);
        if (wnd == NULL) {
                return NULL;
        }

        ShowWindow(wnd, SW_HIDE);
        UpdateWindow(wnd);

        return wnd;
}

static void main_thread_wnd_process(int blocking)
{
        MSG msg;

        while (1) {
                if (blocking) {
                        GetMessage(&msg, NULL, 0, 0);
                } else {
                        PeekMessage(&msg, NULL, 0, 0, PM_REMOVE);
                }

                if (msg.message == WM_QUIT)
                        break;


                TranslateMessage(&msg);
                DispatchMessage(&msg);
        }
}

static int output_path_set(void)
{
        char *workdir = g_config.workdir;
        char *fmt = g_config.output_fmt;
        int err;

        if (workdir[0] == '\0')
                workdir = DEFAULT_WORK_PATH;

        if (fmt[0] == '\0')
                fmt = DEFAULT_OUTPUT_FMT;

        snprintf(out_path, sizeof(out_path), "%s/wallpaper_generated.%s", workdir, fmt);

        if ((err = iconv_utf82wc(out_path, sizeof(out_path), out_path_w, sizeof(out_path_w))))
                return err;

#ifdef UNICODE
        pr_rawlvl(INFO, "output path: \"%ls\"\n", out_path_w);
#else
        pr_rawlvl(INFO, "output path: \"%s\"\n", out_path);
#endif

        return 0;
}

int wmain(int wargc, wchar_t *wargv[])
{
        HWND notify_wnd = NULL;
        int err = 0;

        setbuf(stdout, NULL);

        if ((err = wchar_longopts_parse(wargc, wargv, opt_list)))
                return err;

        if ((err = usrcfg_init()))
                return err;

        if ((err = output_path_set()))
                goto exit_usrcfg;

        InitializeMagick(NULL);

        if (NULL == (notify_wnd = notify_wnd_create()))
                goto exit_magick;

        wallpaper_update();

        main_thread_wnd_process(1);

        DestroyWindow(notify_wnd);

exit_magick:
        DestroyMagick();

exit_usrcfg:
        usrcfg_deinit();

        return err;
}