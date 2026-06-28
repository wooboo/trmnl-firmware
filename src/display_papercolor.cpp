#if defined(BOARD_M5STACK_PAPERCOLOR)

#include <Arduino.h>
#include <LittleFS.h>
#include <M5GFX.h>
#include <M5PM1.h>
#include <M5Unified.h>
#include <display.h>
#include <power.h>
#include <ctype.h>

#include "config.h"
#include "trmnl_log.h"

#define FS LittleFS

static constexpr uint16_t PAPER_COLOR_G5_MARKER = 0xBBBF;
static constexpr m5pm1_gpio_num_t PAPER_COLOR_EPD_EN = M5PM1_GPIO_NUM_0;

static M5Canvas *paper_canvas = nullptr;
static M5PM1 paper_pm1;
static bool paper_pm1_ready = false;
static bool paper_light_sleep_enabled = true;

static bool paper_is_png(const uint8_t *data, int size)
{
    return data && size >= 8 &&
           data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' &&
           data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
}

static bool paper_is_jpeg(const uint8_t *data, int size)
{
    return data && size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

static bool paper_is_bmp(const uint8_t *data, int size)
{
    return data && size >= 2 && data[0] == 'B' && data[1] == 'M';
}

static uint16_t paper_read_le16(const uint8_t *data, size_t offset)
{
    return static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
}

static uint32_t paper_read_le32(const uint8_t *data, size_t offset)
{
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

static bool paper_is_six_color_indexed_bmp(const uint8_t *data, int size)
{
    if (!paper_is_bmp(data, size) || size < 54)
    {
        return false;
    }

    const uint32_t data_offset = paper_read_le32(data, 10);
    const uint32_t dib_size = paper_read_le32(data, 14);
    if (dib_size < 40 || data_offset > static_cast<uint32_t>(size) || data_offset < 14 + dib_size)
    {
        return false;
    }

    const uint16_t planes = paper_read_le16(data, 26);
    const uint16_t bit_count = paper_read_le16(data, 28);
    const uint32_t compression = paper_read_le32(data, 30);
    const uint32_t colors_used = paper_read_le32(data, 46);
    const uint32_t palette_entries = colors_used != 0
                                         ? colors_used
                                         : ((data_offset - (14 + dib_size)) / 4);

    return planes == 1 && bit_count == 4 && compression == 0 && palette_entries == 6;
}

static bool paper_is_g5(const uint8_t *data, int size)
{
    if (!data || size < 2)
    {
        return false;
    }
    return static_cast<uint16_t>(data[0] | (data[1] << 8)) == PAPER_COLOR_G5_MARKER;
}

static bool paper_ensure_canvas()
{
    if (paper_canvas)
    {
        return true;
    }

    paper_canvas = new M5Canvas(&M5.Display);
    if (!paper_canvas)
    {
        Log_error("Failed to allocate M5Canvas");
        return false;
    }

    if (!paper_canvas->createSprite(M5.Display.width(), M5.Display.height()))
    {
        Log_error("Failed to allocate PaperColor sprite %dx%d", M5.Display.width(), M5.Display.height());
        delete paper_canvas;
        paper_canvas = nullptr;
        return false;
    }

    return true;
}

static void paper_clear()
{
    if (!paper_ensure_canvas())
    {
        return;
    }
    paper_canvas->fillSprite(TFT_WHITE);
    paper_canvas->setTextColor(TFT_BLACK, TFT_WHITE);
    paper_canvas->setTextSize(2);
}

static void paper_push(bool wait = true)
{
    if (!paper_canvas)
    {
        return;
    }
    paper_canvas->pushSprite(0, 0);
    if (wait)
    {
        M5.Display.waitDisplay();
    }
}

static void paper_draw_centered_line(const String &text, int y, uint8_t text_size = 2, uint32_t color = TFT_BLACK)
{
    if (!paper_ensure_canvas())
    {
        return;
    }
    paper_canvas->setTextSize(text_size);
    paper_canvas->setTextColor(color, TFT_WHITE);
    int x = (display_width() - paper_canvas->textWidth(text)) / 2;
    if (x < 0)
    {
        x = 0;
    }
    paper_canvas->setCursor(x, y);
    paper_canvas->print(text);
}

static void paper_draw_multiline(int x_start, int y_start, const String &message, uint16_t max_width, bool centered)
{
    if (!paper_ensure_canvas() || message.isEmpty())
    {
        return;
    }

    paper_canvas->setTextSize(2);
    paper_canvas->setTextColor(TFT_BLACK, TFT_WHITE);

    const int line_height = paper_canvas->fontHeight() + 8;
    int y = y_start;
    String line;
    String word;

    auto flush_line = [&]() {
        if (line.isEmpty())
        {
            return;
        }
        int x = x_start;
        if (centered)
        {
            x = (display_width() - paper_canvas->textWidth(line)) / 2;
            if (x < 0)
            {
                x = 0;
            }
        }
        paper_canvas->setCursor(x, y);
        paper_canvas->print(line);
        y += line_height;
        line = "";
    };

    for (size_t i = 0; i <= message.length(); ++i)
    {
        char c = (i < message.length()) ? message[i] : ' ';
        if (c == '\n')
        {
            if (!word.isEmpty())
            {
                if (!line.isEmpty())
                {
                    line += ' ';
                }
                line += word;
                word = "";
            }
            flush_line();
            continue;
        }

        if (isspace(static_cast<unsigned char>(c)) || i == message.length())
        {
            if (!word.isEmpty())
            {
                String candidate = line.isEmpty() ? word : line + " " + word;
                if (!line.isEmpty() && paper_canvas->textWidth(candidate) > max_width)
                {
                    flush_line();
                    line = word;
                }
                else
                {
                    line = candidate;
                }
                word = "";
            }
        }
        else
        {
            word += c;
        }
    }
    flush_line();
}

static const char *paper_message_title(MSG message_type)
{
    switch (message_type)
    {
    case FRIENDLY_ID:
        return "Finish setup";
    case WIFI_CONNECT:
        return "Connect to TRMNL WiFi";
    case WIFI_FAILED:
        return "WiFi failed";
    case WIFI_WEAK:
        return "Weak WiFi signal";
    case WIFI_INTERNAL_ERROR:
        return "WiFi error";
    case WIFI_IMAGE_TIMEOUT:
        return "Image timed out";
    case WIFI_RETRY_LIMIT:
        return "WiFi retry limit";
    case CAPTIVE_WIFI_TIMEOUT:
        return "Setup timed out";
    case API_ERROR:
        return "API error";
    case API_REQUEST_FAILED:
        return "API request failed";
    case API_SIZE_ERROR:
        return "Image size error";
    case API_UNABLE_TO_CONNECT:
        return "Unable to connect";
    case API_SETUP_FAILED:
        return "Setup failed";
    case API_IMAGE_DOWNLOAD_ERROR:
        return "Image download failed";
    case API_FIRMWARE_UPDATE_ERROR:
        return "Firmware update error";
    case FW_UPDATE:
        return "Firmware update";
    case FW_UPDATE_FAILED:
        return "Firmware update failed";
    case FW_UPDATE_SUCCESS:
        return "Firmware update complete";
    case MSG_FORMAT_ERROR:
        return "Unsupported image";
    case MSG_TOO_BIG:
        return "Image too large";
    case MAC_NOT_REGISTERED:
        return "Device not registered";
    case OTG_TURNED_ON:
        return "OTG turned on";
    case OTG_TURNED_OFF:
        return "OTG turned off";
    case READY_TO_SHIP:
        return "Ready to ship";
    case SHIPPING_MODE:
        return "Shipping mode";
    case WIFI_RESET_CONFIRM:
        return "WiFi reset";
    case POWER_OFF_CONFIRM:
        return "Power off";
    case FILL_WHITE:
    case NONE:
        return "";
    default:
        return "TRMNL";
    }
}

static bool paper_draw_image(uint8_t *image_buffer, int data_size)
{
    if (!paper_ensure_canvas() || !image_buffer || data_size <= 0)
    {
        return false;
    }

    bool ok = false;
    if (paper_is_png(image_buffer, data_size))
    {
        ok = paper_canvas->drawPng(image_buffer, data_size, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f);
    }
    else if (paper_is_jpeg(image_buffer, data_size))
    {
        ok = paper_canvas->drawJpg(image_buffer, data_size, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f);
    }
    else if (paper_is_bmp(image_buffer, data_size))
    {
        ok = paper_canvas->drawBmp(image_buffer, data_size, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f);
    }
    else if (paper_is_g5(image_buffer, data_size))
    {
        Log_info("PaperColor does not render Group5 built-in logo assets");
    }

    if (!ok)
    {
        Log_error_serial("PaperColor image decode failed or unsupported; size=%d", data_size);
    }
    return ok;
}

static void paper_draw_status(MSG message_type, const String &primary, const String &secondary)
{
    paper_clear();

    if (message_type == FILL_WHITE)
    {
        paper_push();
        return;
    }

    paper_draw_centered_line("TRMNL", 42, 3);

    String title = primary.isEmpty() ? String(paper_message_title(message_type)) : primary;
    if (!title.isEmpty())
    {
        paper_draw_centered_line(title, 132, 2);
    }

    if (!secondary.isEmpty())
    {
        paper_draw_multiline(40, 190, secondary, display_width() - 80, true);
    }

    paper_push();
}

void display_init(void)
{
    Log_info("M5Stack PaperColor display init start");

    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.clear_display = false;
    M5.begin(cfg);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.setRotation(3);

    if (!paper_ensure_canvas())
    {
        Log_error("PaperColor canvas init failed");
    }

    if (paper_pm1.begin(&M5.In_I2C, M5PM1_DEFAULT_ADDR, M5PM1_I2C_FREQ_100K) == M5PM1_OK)
    {
        paper_pm1_ready = true;
        paper_pm1.setI2cConfig(0);
        paper_pm1.pinMode(PAPER_COLOR_EPD_EN, OUTPUT);
        paper_pm1.digitalWrite(PAPER_COLOR_EPD_EN, HIGH);
        paper_pm1.setChargeEnable(true);
        paper_pm1.setBoostEnable(true);
        Log_info("M5PM1 initialized");
    }
    else
    {
        paper_pm1_ready = false;
        Log_error("M5PM1 init failed");
    }

    Log_info("M5Stack PaperColor display init end");
}

uint8_t tca9535_interrupt_clear()
{
    return 0;
}

void config_bma530_interrupt() {}

void config_tca95535_pins_for_lp() {}

void BQ27427_reset() {}

void otg_turn_on()
{
    Log_info("OTG is not implemented on M5Stack PaperColor");
}

void otg_turn_off()
{
    Log_info("OTG is not implemented on M5Stack PaperColor");
}

battery_count_t detect_battery_count()
{
    return BATTERY_ONE;
}

void enter_shipment_sleep()
{
    if (paper_pm1_ready)
    {
        paper_pm1.sysCmd(M5PM1_SYS_CMD_OFF);
    }
    display_sleep();
}

void display_show_battery(float f)
{
    paper_draw_status(TEST, "Battery", String(f, 3) + " V");
}

void display_reset(void)
{
    paper_clear();
    paper_push();
}

uint16_t display_height()
{
    return M5.Display.height();
}

uint16_t display_width()
{
    return M5.Display.width();
}

void Paint_DrawMultilineText(UWORD x_start, UWORD y_start, const char *message,
                             uint16_t max_width, uint16_t font_width,
                             UWORD color_fg, UWORD color_bg, void *font,
                             bool is_center_aligned)
{
    (void)font_width;
    (void)color_fg;
    (void)color_bg;
    (void)font;
    paper_draw_multiline(x_start, y_start, message ? String(message) : String(), max_width, is_center_aligned);
}

void display_show_image(uint8_t *image_buffer, int data_size, bool bWait)
{
    const bool pre_dithered_palette_bmp = paper_is_six_color_indexed_bmp(image_buffer, data_size);
    const epd_mode_t previous_mode = M5.Display.getEpdMode();
    if (pre_dithered_palette_bmp && previous_mode != epd_mode_t::epd_fastest)
    {
        Log_info("PaperColor 6-color indexed BMP detected; disabling panel-side dithering");
        M5.Display.setEpdMode(epd_mode_t::epd_fastest);
    }

    paper_clear();
    if (!paper_draw_image(image_buffer, data_size))
    {
        Log_info("PaperColor image skipped; leaving current panel image unchanged");
        if (pre_dithered_palette_bmp && previous_mode != M5.Display.getEpdMode())
        {
            M5.Display.setEpdMode(previous_mode);
        }
        return;
    }
    paper_push(bWait);
    if (pre_dithered_palette_bmp && previous_mode != M5.Display.getEpdMode())
    {
        M5.Display.setEpdMode(previous_mode);
    }
}

void papercolor_show_native_color_card(void)
{
    if (!paper_ensure_canvas())
    {
        return;
    }

    struct Patch
    {
        const char *label;
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    static constexpr Patch patches[] = {
        {"0 black", 0, 0, 0},
        {"1 white", 255, 255, 255},
        {"2 yellow", 255, 243, 56},
        {"3 red", 191, 0, 0},
        {"5 blue", 100, 64, 255},
        {"6 green", 67, 138, 28},
    };

    Log_info("PaperColor native color diagnostic card");
    const epd_mode_t previous_mode = M5.Display.getEpdMode();
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);

    paper_canvas->fillSprite(TFT_WHITE);
    const int patch_count = sizeof(patches) / sizeof(patches[0]);
    const int patch_width = display_width() / patch_count;
    const int patch_height = display_height() - 58;

    for (int i = 0; i < patch_count; ++i)
    {
        const int x = i * patch_width;
        const int w = (i == patch_count - 1) ? display_width() - x : patch_width;
        const Patch &patch = patches[i];
        paper_canvas->fillRect(x, 0, w, patch_height, lgfx::color888(patch.r, patch.g, patch.b));
        Log_info("PaperColor native patch %s rgb=%u,%u,%u", patch.label, patch.r, patch.g, patch.b);
    }

    paper_canvas->fillRect(0, patch_height, display_width(), display_height() - patch_height, TFT_WHITE);
    paper_canvas->setTextSize(1);
    paper_canvas->setTextColor(TFT_BLACK, TFT_WHITE);
    for (int i = 0; i < patch_count; ++i)
    {
        const int x = i * patch_width + 4;
        paper_canvas->setCursor(x, patch_height + 8);
        paper_canvas->print(patches[i].label);
    }
    paper_canvas->setCursor(4, patch_height + 32);
    paper_canvas->print("native RGB -> ED2208, no BMP/BYOS");

    paper_push(true);
    if (previous_mode != M5.Display.getEpdMode())
    {
        M5.Display.setEpdMode(previous_mode);
    }
}

uint8_t *display_read_file(const char *filename, int *file_size)
{
    if (!filename || !file_size)
    {
        return nullptr;
    }

    File file = FS.open(filename, "r");
    if (!file)
    {
        Log_error("Failed to open display file: %s", filename);
        return nullptr;
    }

    *file_size = file.size();
    uint8_t *buffer = static_cast<uint8_t *>(ps_malloc(*file_size));
    if (!buffer)
    {
        buffer = static_cast<uint8_t *>(malloc(*file_size));
    }

    if (!buffer)
    {
        Log_error("Failed to allocate %d bytes for %s", *file_size, filename);
        file.close();
        return nullptr;
    }

    size_t bytes_read = file.read(buffer, *file_size);
    file.close();
    if (bytes_read != static_cast<size_t>(*file_size))
    {
        Log_error("Short read for %s: %u/%d", filename, static_cast<unsigned>(bytes_read), *file_size);
        free(buffer);
        return nullptr;
    }

    return buffer;
}

void display_show_msg(uint8_t *image_buffer, MSG message_type, const char *message_text)
{
    (void)image_buffer;
    String secondary = message_text ? String(message_text) : String();
    paper_draw_status(message_type, "", secondary);
}

void display_show_msg(uint8_t *image_buffer, MSG message_type, String friendly_id, bool id, const char *fw_version, String message)
{
    (void)image_buffer;
    String secondary;

    switch (message_type)
    {
    case FRIENDLY_ID:
        secondary = "Visit trmnl.com/start";
        if (id)
        {
            secondary += "\nFriendly ID: " + friendly_id;
        }
        if (!message.isEmpty())
        {
            secondary += "\n" + message;
        }
        break;
    case WIFI_CONNECT:
        secondary = "Connect your phone or computer to the TRMNL WiFi.";
        if (fw_version && fw_version[0] != '\0')
        {
            secondary += "\nFirmware " + String(fw_version);
        }
        break;
    case MAC_NOT_REGISTERED:
        secondary = message;
        break;
    default:
        secondary = message;
        break;
    }

    paper_draw_status(message_type, "", secondary);
}

void display_show_msg_api(uint8_t *image_buffer, String message)
{
    (void)image_buffer;
    paper_draw_status(API_ERROR, "", message);
}

void display_show_msg_qa(uint8_t *image_buffer, const float *voltage, const float *temperature, bool qa_result)
{
    (void)image_buffer;
    String secondary;
    if (voltage)
    {
        secondary += "Initial voltage: " + String(voltage[0], 4) + " V\n";
        secondary += "Final voltage: " + String(voltage[1], 4) + " V\n";
        secondary += "Diff: " + String(voltage[2], 4) + " V\n";
    }
    if (temperature)
    {
        secondary += "Initial temperature: " + String(temperature[0], 4) + " C\n";
        secondary += "Final temperature: " + String(temperature[1], 4) + " C\n";
        secondary += "Diff: " + String(temperature[2], 4) + " C\n";
    }
    secondary += qa_result ? "QA passed" : "QA failed";
    paper_draw_status(QA_START, qa_result ? "PASS" : "FAIL", secondary);
}

void display_set_light_sleep(uint8_t enabled)
{
    paper_light_sleep_enabled = enabled != 0;
}

void display_sleep(void)
{
    Log_info("PaperColor display sleep; light_sleep_enabled=%d", paper_light_sleep_enabled);
    M5.Display.waitDisplay();
    M5.Display.sleep();
    if (paper_pm1_ready)
    {
        paper_pm1.digitalWrite(PAPER_COLOR_EPD_EN, LOW);
    }
}

float papercolor_read_battery_voltage(void)
{
    if (!paper_pm1_ready)
    {
        Log_error("M5PM1 not initialized; cannot read battery voltage");
        return -1.0f;
    }

    uint16_t battery_mv = 0;
    if (paper_pm1.readVbat(&battery_mv) != M5PM1_OK)
    {
        Log_error("M5PM1 readVbat failed");
        return -1.0f;
    }

    float voltage = battery_mv / 1000.0f;
    Log_info("Battery voltage reading from M5PM1: %.3f V", voltage);
    return voltage;
}

UsbStatus papercolor_get_usb_status(void)
{
    if (!paper_pm1_ready)
    {
        return UsbStatus::UNKNOWN;
    }

    m5pm1_pwr_src_t source = M5PM1_PWR_SRC_UNKNOWN;
    if (paper_pm1.getPowerSource(&source) != M5PM1_OK)
    {
        return UsbStatus::UNKNOWN;
    }

    if (source == M5PM1_PWR_SRC_5VIN || source == M5PM1_PWR_SRC_5VINOUT)
    {
        return UsbStatus::CONNECTED;
    }

    if (source == M5PM1_PWR_SRC_BAT)
    {
        return UsbStatus::DISCONNECTED;
    }

    return UsbStatus::UNKNOWN;
}

ChargingStatus papercolor_get_charging_status(void)
{
    return ChargingStatus::UNKNOWN;
}

#endif // BOARD_M5STACK_PAPERCOLOR
