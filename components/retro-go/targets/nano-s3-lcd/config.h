// Target definition
#define RG_TARGET_NAME "NANO-S3-LCD"

// Storage
#define RG_STORAGE_ROOT        "/sd"
#define RG_STORAGE_SDMMC_HOST  SDMMC_HOST_SLOT_1
#define RG_STORAGE_SDMMC_SPEED SDMMC_FREQ_DEFAULT
// #define RG_STORAGE_FLASH_PARTITION  "vfs"

// Audio
#define RG_AUDIO_USE_INT_DAC 0 // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC 1 // 0 = Disable, 1 = Enable


// Video
#define RG_SCREEN_DRIVER    0 // 0 = ILI9341/ST7789
#define RG_SCREEN_HOST      SPI2_HOST
#define RG_SCREEN_SPEED     SPI_MASTER_FREQ_40M
#define RG_SCREEN_BACKLIGHT 1
#define RG_SCREEN_WIDTH     240
#define RG_SCREEN_HEIGHT    240
// #define RG_SCREEN_ST7789_240X240
#define RG_SCREEN_ROTATE       0
#define RG_SCREEN_VISIBLE_AREA {0, 0, 0, 0} // Left, Top, Right, Bottom
#define RG_SCREEN_SAFE_AREA    {0, 0, 0, 0} // Left, Top, Right, Bottom
#define RG_SCREEN_INIT()                             \
    ILI9341_CMD(0x36, 0x00);                         \
    ILI9341_CMD(0x3A, 0x05);                         \
    ILI9341_CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33); \
    ILI9341_CMD(0xB4, 0x01);                         \
    ILI9341_CMD(0xC0, 0x2C, 0x2D);                   \
    ILI9341_CMD(0xC5, 0x2E);                         \
    ILI9341_CMD(0x21);                               \
    ILI9341_CMD(0x2A, 0x00, 0x00, 0x00, 0xEF);       \
    ILI9341_CMD(0x2B, 0x00, 0x00, 0x00, 0xEF);       \
    ILI9341_CMD(0x11);                               \
    ILI9341_CMD(0x29);


#define RG_GAMEPAD_GPIO_MAP                                          \
    {                                                                \
        {RG_KEY_UP,     .num = GPIO_NUM_1,  .pullup = 1, .level = 0},     \
        {RG_KEY_DOWN,   .num = GPIO_NUM_2,  .pullup = 1, .level = 0},   \
        {RG_KEY_LEFT,   .num = GPIO_NUM_3,  .pullup = 1, .level = 0},   \
        {RG_KEY_RIGHT,  .num = GPIO_NUM_4,  .pullup = 1, .level = 0},  \
        {RG_KEY_A,      .num = GPIO_NUM_5,  .pullup = 1, .level = 0},      \
        {RG_KEY_B,      .num = GPIO_NUM_7,  .pullup = 1, .level = 0},      \
        {RG_KEY_SELECT, .num = GPIO_NUM_8,  .pullup = 1, .level = 0}, \
        {RG_KEY_START,  .num = GPIO_NUM_9,  .pullup = 1, .level = 0},  \
        {RG_KEY_MENU,   .num = GPIO_NUM_10, .pullup = 1, .level = 0},  \
        {RG_KEY_OPTION, .num = GPIO_NUM_11, .pullup = 1, .level = 0} \
}


// Battery
#define RG_BATTERY_DRIVER            1
#define RG_BATTERY_ADC_UNIT          ADC_UNIT_1
#define RG_BATTERY_ADC_CHANNEL       ADC_CHANNEL_5 // GPIO 6
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f * 3300.f / 4095.f - 3300.f) / (4200.f - 3300.f) * 100.f)
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 3.3f / 4095.f)


// Status LED
// #define RG_GPIO_LED                 GPIO_NUM_43

// I2C BUS
#define RG_GPIO_I2C_SDA GPIO_NUM_47
#define RG_GPIO_I2C_SCL GPIO_NUM_48

// SPI Display
#define RG_GPIO_LCD_MISO GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI GPIO_NUM_41
#define RG_GPIO_LCD_CLK  GPIO_NUM_40
#define RG_GPIO_LCD_CS   GPIO_NUM_39
#define RG_GPIO_LCD_DC   GPIO_NUM_38
#define RG_GPIO_LCD_BCKL GPIO_NUM_20 // Using PWM pin from reference
#define RG_GPIO_LCD_RST  GPIO_NUM_42


// SD Card (SDMMC 1-bit mode)
#define RG_GPIO_SDSPI_CLK GPIO_NUM_21
#define RG_GPIO_SDSPI_CMD GPIO_NUM_18
#define RG_GPIO_SDSPI_D0  GPIO_NUM_16


// External I2S DAC
#define RG_GPIO_SND_I2S_BCK    GPIO_NUM_12 // Note: Overlaps with GAMEPAD if not careful, but reference uses it.
#define RG_GPIO_SND_I2S_WS     GPIO_NUM_13
#define RG_GPIO_SND_I2S_DATA   GPIO_NUM_14
#define RG_GPIO_SND_AMP_ENABLE GPIO_NUM_NC


// Updater
#define RG_UPDATER_ENABLE            1
#define RG_UPDATER_APPLICATION       RG_APP_FACTORY
#define RG_UPDATER_DOWNLOAD_LOCATION RG_STORAGE_ROOT "/nano-s3/firmware"
