#ifndef APP_VERSION_H
#define APP_VERSION_H
/* 固件版本：关于页显示 + 未来 OTA 比对用。发版时改这里。 */
#define FW_VERSION_MAJOR   1
#define FW_VERSION_MINOR   1
#define FW_VERSION_PATCH   0
#define FW_VERSION_STRING  "1.1.0"
#define FW_BUILD_DATE      __DATE__
#define FW_BUILD_TIME      __TIME__

/* 硬件/平台标识（关于页展示） */
#define HW_BOARD_NAME      "QM F407ZGT6 V4.1"
#define LCD_MODEL_NAME     "ILI9341 2.8 TFT + XPT2046"

#endif
