#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <stdio.h>
#include <string.h>

#include "appfs.h"
#include "appfs_wrapper.h"
#include "filesystems.h"
#include "graphics_wrapper.h"
#include "ili9341.h"
#include "menu.h"
#include "metadata.h"
#include "pax_codecs.h"
#include "pax_gfx.h"
#include "rp2040.h"
#include "system_wrapper.h"

extern const uint8_t apps_png_start[] asm("_binary_apps_png_start");
extern const uint8_t apps_png_end[] asm("_binary_apps_png_end");

static const char* sdcard_path   = "/sd/apps/esp32/";
static const char* internal_path = "/internal/apps/esp32/";

static bool populate(menu_t* menu) {
    size_t previous_position = menu_get_position(menu);
    for (size_t index = 0; index < menu_get_length(menu); index++) {
        free(menu_get_callback_args(menu, index));
    }
    while (menu_remove_item(menu, 0)) {
        ;
    }

    bool           empty    = true;
    appfs_handle_t appfs_fd = appfsNextEntry(APPFS_INVALID_FD);
    while (appfs_fd != APPFS_INVALID_FD) {
        empty               = false;
        const char* slug    = NULL;
        const char* title   = NULL;
        uint16_t    version = 0xFFFF;
        appfsEntryInfoExt(appfs_fd, &slug, &title, &version, NULL);
        appfs_handle_t* args = malloc(sizeof(appfs_handle_t));
        *args                = appfs_fd;
        char icon_file_path[257];
        icon_file_path[sizeof(icon_file_path) - 1] = '\0';
        snprintf(icon_file_path, sizeof(icon_file_path) - 1, "%s/%s/icon.png", internal_path, slug);

        FILE* icon_fd = fopen(icon_file_path, "rb");
        if (icon_fd != NULL) {
            pax_buf_t* icon      = NULL;
            size_t     icon_size = get_file_size(icon_fd);
            uint8_t*   icon_data = load_file_to_ram(icon_fd);
            if (icon_data != NULL) {
                icon = malloc(sizeof(pax_buf_t));
                if (icon != NULL) {
                    pax_decode_png_buf(icon, (void*) icon_data, icon_size, PAX_BUF_32_8888ARGB, 0);
                }
                free(icon_data);
            }
            fclose(icon_fd);
            menu_insert_item_icon(menu, title, NULL, (void*) args, -1, icon);
        } else {
            menu_insert_item(menu, title, NULL, (void*) args, -1);
        }
        appfs_fd = appfsNextEntry(appfs_fd);
    }

    menu_set_position(menu, previous_position);
    return empty;
}

typedef enum { CONTEXT_ACTION_NONE, CONTEXT_ACTION_UNINSTALL } context_menu_action_t;

void context_menu(appfs_handle_t fd, xQueueHandle buttonQueue, pax_buf_t* pax_buffer, ILI9341* ili9341) {
    const char* name    = NULL;
    const char* title   = NULL;
    uint16_t    version = 0xFFFF;
    appfsEntryInfoExt(fd, &name, &title, &version, NULL);
    menu_t* menu = menu_alloc(title, 20, 18);

    menu->fgColor           = 0xFF000000;
    menu->bgColor           = 0xFFFFFFFF;
    menu->bgTextColor       = 0xFFFFFFFF;
    menu->selectedItemColor = 0xFF491d88;
    menu->borderColor       = 0xFF43b5a0;
    menu->titleColor        = 0xFF491d88;
    menu->titleBgColor      = 0xFF43b5a0;
    menu->scrollbarBgColor  = 0xFFCCCCCC;
    menu->scrollbarFgColor  = 0xFF555555;

    menu_insert_item(menu, "Uninstall", NULL, (void*) CONTEXT_ACTION_UNINSTALL, -1);

    bool render = true;
    bool quit   = false;
    while (!quit) {
        context_menu_action_t  action        = CONTEXT_ACTION_NONE;
        rp2040_input_message_t buttonMessage = {0};
        if (xQueueReceive(buttonQueue, &buttonMessage, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
            if (buttonMessage.state) {
                switch (buttonMessage.input) {
                    case RP2040_INPUT_JOYSTICK_DOWN:
                        menu_navigate_next(menu);
                        render = true;
                        break;
                    case RP2040_INPUT_JOYSTICK_UP:
                        menu_navigate_previous(menu);
                        render = true;
                        break;
                    case RP2040_INPUT_BUTTON_ACCEPT:
                    case RP2040_INPUT_JOYSTICK_PRESS:
                        action = (context_menu_action_t) menu_get_callback_args(menu, menu_get_position(menu));
                        break;
                    case RP2040_INPUT_BUTTON_HOME:
                    case RP2040_INPUT_BUTTON_BACK:
                        quit = true;
                        break;
                    case RP2040_INPUT_BUTTON_MENU:
                        break;
                    case RP2040_INPUT_BUTTON_SELECT:
                    case RP2040_INPUT_BUTTON_START:
                    default:
                        break;
                }
            }
        }
        if (render) {
            menu_render(pax_buffer, menu, 20, 20, 280, 180);
            ili9341_write(ili9341, pax_buffer->buf);
            render = false;
        }

        if (action == CONTEXT_ACTION_UNINSTALL) {
            render_message(pax_buffer, "Uninstalling app...");
            ili9341_write(ili9341, pax_buffer->buf);
            appfsDeleteFile(name);
            quit = true;
        }
    }

    menu_free(menu);
}

void menu_launcher_esp32(xQueueHandle buttonQueue, pax_buf_t* pax_buffer, ILI9341* ili9341) {
    pax_noclip(pax_buffer);
    menu_t* menu            = menu_alloc("ESP32 apps", 34, 18);
    menu->fgColor           = 0xFF000000;
    menu->bgColor           = 0xFFFFFFFF;
    menu->bgTextColor       = 0xFFFFFFFF;
    menu->selectedItemColor = 0xFFfa448c;
    menu->borderColor       = 0xFF491d88;
    menu->titleColor        = 0xFFfa448c;
    menu->titleBgColor      = 0xFF491d88;
    menu->scrollbarBgColor  = 0xFFCCCCCC;
    menu->scrollbarFgColor  = 0xFF555555;

    pax_buf_t icon_apps;
    pax_decode_png_buf(&icon_apps, (void*) apps_png_start, apps_png_end - apps_png_start, PAX_BUF_32_8888ARGB, 0);
    menu_set_icon(menu, &icon_apps);

    const pax_font_t* font = pax_font_saira_regular;

    bool empty = populate(menu);

    bool            render                = true;
    appfs_handle_t* appfs_fd_to_start     = NULL;
    bool            quit                  = false;
    appfs_handle_t* appfs_fd_context_menu = NULL;
    while (!quit) {
        rp2040_input_message_t buttonMessage = {0};
        if (xQueueReceive(buttonQueue, &buttonMessage, 1000 / portTICK_PERIOD_MS) == pdTRUE) {
            if (buttonMessage.state) {
                switch (buttonMessage.input) {
                    case RP2040_INPUT_JOYSTICK_DOWN:
                        menu_navigate_next(menu);
                        render = true;
                        break;
                    case RP2040_INPUT_JOYSTICK_UP:
                        menu_navigate_previous(menu);
                        render = true;
                        break;
                    case RP2040_INPUT_BUTTON_ACCEPT:
                    case RP2040_INPUT_JOYSTICK_PRESS:
                        appfs_fd_to_start = (appfs_handle_t*) menu_get_callback_args(menu, menu_get_position(menu));
                        break;
                    case RP2040_INPUT_BUTTON_HOME:
                    case RP2040_INPUT_BUTTON_BACK:
                        quit = true;
                        break;
                    case RP2040_INPUT_BUTTON_MENU:
                        appfs_fd_context_menu = (appfs_handle_t*) menu_get_callback_args(menu, menu_get_position(menu));
                        break;
                    case RP2040_INPUT_BUTTON_SELECT:
                    case RP2040_INPUT_BUTTON_START:
                    default:
                        break;
                }
            }
        }

        if (appfs_fd_context_menu != NULL) {
            context_menu(*appfs_fd_context_menu, buttonQueue, pax_buffer, ili9341);
            empty                 = populate(menu);
            appfs_fd_context_menu = NULL;
            render                = true;
        }

        if (render) {
            pax_background(pax_buffer, 0xFFFFFF);
            pax_draw_text(pax_buffer, 0xFF491d88, font, 18, 5, 240 - 18, "🅰 start app  🅱 back  🅼 options");
            menu_render(pax_buffer, menu, 0, 0, 320, 220);
            if (empty) render_message(pax_buffer, "No apps installed");
            ili9341_write(ili9341, pax_buffer->buf);
            render = false;
        }

        if (appfs_fd_to_start != NULL) {
            appfs_boot_app(*appfs_fd_to_start);
            break;
        }
    }

    for (size_t index = 0; index < menu_get_length(menu); index++) {
        pax_buf_t* icon = menu_get_icon(menu, index);
        if (icon != NULL) pax_buf_destroy(icon);
        free(menu_get_callback_args(menu, index));
    }

    menu_free(menu);
    pax_buf_destroy(&icon_apps);
}
