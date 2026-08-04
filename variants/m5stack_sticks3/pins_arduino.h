// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>
#include "soc/soc_caps.h"

#define USB_VID          0x303a
#define USB_PID          0x1001
#define USB_MANUFACTURER "M5Stack"
#define USB_PRODUCT      "StickS3"

// Some StickS3 boards do not drive the built-in LED reliably on GPIO48.
static const uint8_t LED_BUILTIN = SOC_GPIO_PIN_COUNT + 48;
#define BUILTIN_LED    LED_BUILTIN
#define LED_BUILTIN    LED_BUILTIN
#define RGB_BUILTIN    LED_BUILTIN
#define RGB_BRIGHTNESS 64

static const uint8_t TX = 43;
static const uint8_t RX = 44;

static const uint8_t TXD2 = 1;
static const uint8_t RXD2 = 2;

static const uint8_t SCL = 10;
static const uint8_t SDA = 9;

static const uint8_t EX_SCL = 10;
static const uint8_t EX_SDA = 9;

static const uint8_t IN_SCL = 48;
static const uint8_t IN_SDA = 47;

static const uint8_t SS   = 41;
static const uint8_t MOSI = 39;
static const uint8_t MISO = -1;
static const uint8_t SCK  = 40;

static const uint8_t G0  = 0;
static const uint8_t G1  = 1;
static const uint8_t G2  = 2;
static const uint8_t G3  = 3;
static const uint8_t G4  = 4;
static const uint8_t G5  = 5;
static const uint8_t G6  = 6;
static const uint8_t G7  = 7;
static const uint8_t G8  = 8;
static const uint8_t G36 = 36;
static const uint8_t G37 = 37;
static const uint8_t G38 = 38;
static const uint8_t G39 = 39;
static const uint8_t G40 = 40;
static const uint8_t G42 = 42;

static const uint8_t ADC1 = 7;
static const uint8_t ADC2 = 8;

#endif
