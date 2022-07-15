#include <stdio.h>
#include <stdlib.h>

#include <stdbool.h>
#include <string.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "common.h"
#include "main_page.h"

/* Littlevgl specific */
#ifdef LV_LVGL_H_INCLUDE_SIMPLE

#include "lvgl.h"

#else
#include "lvgl/lvgl.h"
#endif

#include "esp_lcd_panel_ssd1680.h"
#include "epdpaint.h"


/*********************
 *      DEFINES
 *********************/
#define TAG "main-page"
#define LV_TICK_PERIOD_MS 1

#define TFT_SPI_HOST 1
#define DISP_SPI_MISO CONFIG_DISP_SPI_MISO
#define DISP_SPI_MOSI CONFIG_DISP_SPI_MOSI
#define DISP_SPI_CLK CONFIG_DISP_SPI_CLK
#define DISP_PIN_RST CONFIG_DISP_PIN_RST
#define DISP_SPI_CS CONFIG_DISP_SPI_CS
#define DISP_PIN_DC CONFIG_DISP_PIN_DC
#define DISP_PIN_BUSY CONFIG_DISP_PIN_BUSY

#define DISP_BUFF_SIZE LCD_H_RES * LCD_V_RES

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void lv_tick_task(void *arg);

static void guiTask(void *pvParameter);
static void guiTask_EPD_LVGL(void *pvParameter);
static void guiTask_LVGL(void *pvParameter);

static void draw_test_1(lcd_ssd1680_panel_t *panel);
static void draw_test_2(lv_obj_t *scr);

void init_main_page() {
    /* If you want to use a task to create the graphic, you NEED to create a Pinned task
     * Otherwise there can be problem such as memory corruption and so on.
     * NOTE: When not using Wi-Fi nor Bluetooth you can pin the guiTask to core 0 */
    xTaskCreatePinnedToCore(guiTask_EPD_LVGL, "gui", 4096 * 2, NULL, 0, NULL, 1);
}

/* Creates a semaphore to handle concurrent call to lvgl stuff
 * If you wish to call *any* lvgl function from other threads/tasks
 * you should lock on the very same semaphore! */
SemaphoreHandle_t xGuiSemaphore;

bool spi_driver_init(int host,
                     int miso_pin, int mosi_pin, int sclk_pin,
                     int max_transfer_sz,
                     int dma_channel) {

    ESP_LOGI(TAG, "Initialize SPI bus");

    assert((0 <= host));
    const char *spi_names[] = {
            "SPI1_HOST", "SPI2_HOST", "SPI3_HOST"
    };

    ESP_LOGI(TAG, "Configuring SPI host %s [%d]", spi_names[host], host);
    ESP_LOGI(TAG, "MISO pin: %d, MOSI pin: %d, SCLK pin: %d", miso_pin, mosi_pin, sclk_pin);

    ESP_LOGI(TAG, "Max transfer size: %d (bytes)", max_transfer_sz);

    spi_bus_config_t buscfg = {
            .miso_io_num = miso_pin,
            .mosi_io_num = mosi_pin,
            .sclk_io_num = sclk_pin,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = max_transfer_sz
    };

    ESP_LOGI(TAG, "Initializing SPI bus...");
#if defined (CONFIG_IDF_TARGET_ESP32C3)
    dma_channel = SPI_DMA_CH_AUTO;
#elif defined (CONFIG_IDF_TARGET_ESP32S3)
    dma_channel = SPI_DMA_CH_AUTO;
#endif
    ESP_LOGI(TAG, "SPI DMA channel %d", dma_channel);
    esp_err_t ret = spi_bus_initialize(host, &buscfg, (spi_dma_chan_t) dma_channel);
    assert(ret == ESP_OK);

    return ESP_OK != ret;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void lvgl_flush_epd_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    // 是不是最后一块
    // lv_disp_flush_is_last();
    ESP_LOGI(TAG, "lvgl_flush_epd_cb x1:%d, x2:%d, y1:%d, y2:%d", area->x1, area->x2, area->y1, area->y2);
    // x1:0, x2:127, y1:0, y2:295 左右都闭区间

    lcd_ssd1680_panel_t *panel = drv->user_data;
    // copy a buffer's content to a specific area of the display
    panel_ssd1680_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

static void lvgl_set_px_epd_cb(lv_disp_drv_t* disp_drv, uint8_t * buf, lv_coord_t buf_w, lv_coord_t x, lv_coord_t y,
                               lv_color_t color, lv_opa_t opa) {
    if (color.full) {
        buf[(x + y * buf_w) / 8] |= 0x80 >> (x % 8);
    } else {
        buf[(x + y * buf_w) / 8] &= ~(0x80 >> (x % 8));
    }
}

static bool
notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *) user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void guiTask(void *pvParameter) {

    (void) pvParameter;
    xGuiSemaphore = xSemaphoreCreateMutex();

    spi_driver_init(TFT_SPI_HOST,
                    DISP_SPI_MISO, DISP_SPI_MOSI, DISP_SPI_CLK,
                    DISP_BUFF_SIZE * sizeof(uint16_t), SPI_DMA_CH_AUTO);

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
            .dc_gpio_num = DISP_PIN_DC,
            .cs_gpio_num = DISP_SPI_CS,
            .pclk_hz = 400000,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .spi_mode = 0,
            .trans_queue_depth = 10,
            //.on_color_trans_done = ,
    };

    lcd_ssd1680_panel_t panel = {
            .busy_gpio_num = DISP_PIN_BUSY,
            .reset_gpio_num = DISP_PIN_RST,
            .reset_level = 0,
    };

    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(new_panel_ssd1680(&panel, TFT_SPI_HOST, &io_config));

    ESP_LOGI(TAG, "Reset SSD1680 panel driver");
    panel_ssd1680_reset(&panel);

    ESP_LOGI(TAG, "Init SSD1680 panel");
    panel_ssd1680_init(&panel);
    // panel_ssd1680_init_partial(&panel);

    draw_test_1(&panel);

    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Try to take the semaphore, call lvgl related function on success */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
            // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
            xSemaphoreGive(xGuiSemaphore);
        }
    }

    vTaskDelete(NULL);
}

static void guiTask_EPD_LVGL(void *pvParameter) {

    (void) pvParameter;
    xGuiSemaphore = xSemaphoreCreateMutex();

    spi_driver_init(TFT_SPI_HOST,
                    DISP_SPI_MISO, DISP_SPI_MOSI, DISP_SPI_CLK,
                    DISP_BUFF_SIZE * sizeof(uint16_t), SPI_DMA_CH_AUTO);

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
            .dc_gpio_num = DISP_PIN_DC,
            .cs_gpio_num = DISP_SPI_CS,
            .pclk_hz = 400000,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .spi_mode = 0,
            .trans_queue_depth = 10,
            //.on_color_trans_done = ,
    };

    lcd_ssd1680_panel_t panel = {
            .busy_gpio_num = DISP_PIN_BUSY,
            .reset_gpio_num = DISP_PIN_RST,
            .reset_level = 0,
    };

    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(new_panel_ssd1680(&panel, TFT_SPI_HOST, &io_config));

    ESP_LOGI(TAG, "Reset SSD1680 panel driver");
    panel_ssd1680_reset(&panel);

    ESP_LOGI(TAG, "Init SSD1680 panel");
    panel_ssd1680_init(&panel);
    // panel_ssd1680_init_partial(&panel);

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    lv_color_t *buf1 = malloc(DISP_BUFF_SIZE * sizeof(lv_color_t));
    assert(buf1);

    lv_color_t *buf2 = malloc(DISP_BUFF_SIZE * sizeof(lv_color_t));
    assert(buf2);

    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions

    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, DISP_BUFF_SIZE);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_epd_cb;
    disp_drv.set_px_cb = lvgl_set_px_epd_cb;
    //disp_drv.rounder_cb
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = &panel;
    disp_drv.full_refresh = true;
    disp_drv.direct_mode = true;

    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
            .callback = &lv_tick_task,
            .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LV_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Display LVGL Hello World Text!");
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    /* Create the demo application */
    draw_test_2(scr);

    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Try to take the semaphore, call lvgl related function on success */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
            // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
            lv_timer_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
    }

    /* A task should NEVER return */
    free(buf1);
    free(buf2);

    vTaskDelete(NULL);
}

static void guiTask_LVGL(void *pvParameter) {

    (void) pvParameter;
    xGuiSemaphore = xSemaphoreCreateMutex();

    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions

    spi_driver_init(TFT_SPI_HOST,
                    DISP_SPI_MISO, DISP_SPI_MOSI, DISP_SPI_CLK,
                    DISP_BUFF_SIZE * sizeof(uint16_t), SPI_DMA_CH_AUTO);

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
            .dc_gpio_num = DISP_PIN_DC,
            .cs_gpio_num = DISP_SPI_CS,
            .pclk_hz = 400000,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .spi_mode = 0,
            .trans_queue_depth = 10,
            .on_color_trans_done = notify_lvgl_flush_ready,
            .user_ctx = &disp_drv,
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) TFT_SPI_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install SSD1680 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;

    esp_lcd_panel_dev_config_t panel_config = {
            .flags.reset_active_high = 0,
            .reset_gpio_num = DISP_PIN_RST,
#ifdef CONFIG_SPI_DISPLAY_SSD1680
            .color_space = ESP_LCD_COLOR_SPACE_MONOCHROME,
            .bits_per_pixel = 1,
#else
            .color_space = ESP_LCD_COLOR_SPACE_RGB,
            .bits_per_pixel = 16,
#endif
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    // Turn off backlight to avoid unpredictable display on the LCD screen while initializing
    // the LCD panel driver. (Different LCD screens may need different levels)
#ifdef CONFIG_DISP_PIN_BLK
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_DISP_PIN_BLK, 1));
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Turn on the screen
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    // Swap x and y axis (Different LCD screens may need different options)
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));

#ifdef CONFIG_DISP_PIN_BLK
    // Turn on backlight (Different LCD screens may need different levels)
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_DISP_PIN_BLK, 0));
#endif

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    // alloc draw buffers used by LVGL
    // it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    lv_color_t *buf1 = malloc(DISP_BUFF_SIZE * sizeof(lv_color_t));
    assert(buf1);

    lv_color_t *buf2 = malloc(DISP_BUFF_SIZE * sizeof(lv_color_t));
    assert(buf2);
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, DISP_BUFF_SIZE);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb; // Write the internal buffer (draw_buf) to the display. 'lv_disp_flush_ready()' has to be * called when finished
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;

    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
            .callback = &lv_tick_task,
            .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LV_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Display LVGL Hello World Text!");
    lv_obj_t *scr = lv_disp_get_scr_act(disp);

    /* Create the demo application */
    draw_test_2(scr);

    while (1) {
        // raise the task priority of LVGL and/or reduce the handler period can improve the performance
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Try to take the semaphore, call lvgl related function on success */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
            // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
            lv_timer_handler();
            xSemaphoreGive(xGuiSemaphore);
        }
    }

    /* A task should NEVER return */
    free(buf1);
    free(buf2);

    vTaskDelete(NULL);
}

static void draw_test_1(lcd_ssd1680_panel_t *panel) {
    // for test
    epd_paint_t *epd_paint = malloc(sizeof(epd_paint_t));
    uint8_t *image = malloc(sizeof(uint8_t) * LCD_H_RES * LCD_V_RES / 8);

    epd_paint_init(epd_paint, image, LCD_H_RES, LCD_V_RES);

    epd_paint_clear(epd_paint, 1);
    panel_ssd1680_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, epd_paint->image);

    epd_paint_draw_string_at(epd_paint, 0, 0, "abcdefghijk", &Font20, 0);
    epd_paint_draw_string_at(epd_paint, 0, 21, "lmopqrstuv", &Font12, 0);
    epd_paint_draw_string_at(epd_paint, 0, 35, "hello world! 0123456789", &Font8, 0);
    epd_paint_draw_string_at(epd_paint, 0, 45, "hello world! 0123456789", &Font12, 0);
    epd_paint_draw_string_at(epd_paint, 0, 60, "hello world!", &Font16, 0);
    epd_paint_draw_string_at(epd_paint, 0, 78, "hello!012345", &Font20, 0);
    epd_paint_draw_string_at(epd_paint, 0, 100, "hello!", &Font24, 0);
    epd_paint_draw_string_at(epd_paint, 0, 125, "01234ABCD", &Font24, 0);
    epd_paint_draw_string_at(epd_paint, 0, 150, "56789EFGH", &Font24, 0);
    epd_paint_draw_string_at(epd_paint, 0, 175, "IJKLMOPQR", &Font24, 0);
    epd_paint_draw_string_at(epd_paint, 0, 200, "STUVWXYZ", &Font24, 0);
    epd_paint_draw_string_at(epd_paint, 0, 225, "!@$%^&*", &Font24, 0);
    epd_paint_draw_string_at(epd_paint, 0, 250, "()-_=+~`", &Font24, 0);
    epd_paint_draw_string_at(epd_paint, 0, 275, ",.<>/?;]", &Font24, 0);
    panel_ssd1680_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, epd_paint->image);

    panel_ssd1680_sleep(panel);

    epd_paint_deinit(epd_paint);
    free(image);
    free(epd_paint);
}

static void draw_test_2(lv_obj_t *scr) {
    /*Create a Label on the currently active screen*/
    lv_obj_t *label1 = lv_label_create(scr);
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_text_font(&style, &lv_font_montserrat_36);  /*Set a larger font*/
    lv_obj_add_style(label1, &style, LV_STATE_DEFAULT);
    /*Modify the Label's text*/
    lv_label_set_text(label1, "20.5");
    lv_obj_align(label1, LV_ALIGN_TOP_MID, 0, 32);

    lv_obj_t *label2 = lv_label_create(scr);
    lv_label_set_text(label2, "km/h");
    lv_obj_set_style_text_font(label2, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_align_to(label2, label1, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    lv_obj_t *label3 = lv_label_create(scr);
    lv_label_set_text(label3, "ABCDEFGabcdefghij");
    lv_obj_set_style_text_font(label3, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_align_to(label3, label2, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    lv_obj_t *label4 = lv_label_create(scr);
    lv_label_set_text(label4, "ABCDEFGabcdefgHIJ");
    lv_obj_set_style_text_font(label4, &lv_font_montserrat_10, LV_STATE_DEFAULT);
    lv_obj_align_to(label4, label3, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    lv_obj_t *label5 = lv_label_create(scr);
    lv_label_set_text(label5, "ABCDEFGabcdefgHIJ");
    lv_obj_set_style_text_font(label5, &lv_font_montserrat_12, LV_STATE_DEFAULT);
    lv_obj_align_to(label5, label4, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

    lv_obj_t *label6 = lv_label_create(scr);
    lv_label_set_text(label6, "ABCDEFGabcdefgHIJ");
    lv_obj_set_style_text_font(label6, &lv_font_montserrat_14, LV_STATE_DEFAULT);
    lv_obj_align_to(label6, label5, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
}

static void lv_tick_task(void *arg) {
    (void) arg;

    lv_tick_inc(LV_TICK_PERIOD_MS);
}