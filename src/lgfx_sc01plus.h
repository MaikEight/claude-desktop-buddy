#pragma once
#include <LovyanGFX.hpp>

// LovyanGFX configuration for the Sunton SC01 Plus (WT32-SC01 Plus).
// Panel: ST7796 via 8-bit parallel MCU8080 bus (480x320, run in portrait = 320x480).
// Touch: FT5x06-compatible FT6336U via I2C.
// Backlight: GPIO 45, active-high PWM.
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796  _panel;
  lgfx::Bus_Parallel8 _bus;
  lgfx::Light_PWM     _light;
  lgfx::Touch_FT5x06  _touch;

public:
  LGFX() {
    // 8-bit parallel bus
    {
      auto c = _bus.config();
      c.port      = 0;      // LCD_CAM port 0 (ESP32-S3 has one I2S LCD port)
      c.pin_wr    = 47;
      c.pin_rd    = -1;
      c.pin_rs    = 0;      // DC/RS — also BOOT0 strapping pin, released after boot
      c.pin_d0    = 9;
      c.pin_d1    = 46;
      c.pin_d2    = 3;
      c.pin_d3    = 8;
      c.pin_d4    = 18;
      c.pin_d5    = 17;
      c.pin_d6    = 16;
      c.pin_d7    = 15;
      c.freq_write = 16000000;   // 16 MHz — conservative for first bring-up
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    // ST7796 panel — IC native portrait 320x480; setRotation(1) in setup gives landscape 480x320.
    {
      auto c = _panel.config();
      c.pin_cs   = -1;
      c.pin_rst  = 4;
      c.pin_busy = -1;
      c.panel_width  = 320;   // IC native portrait width
      c.panel_height = 480;   // IC native portrait height
      c.offset_x = 0;
      c.offset_y = 0;
      c.offset_rotation = 0;
      c.dummy_read_pixel = 8;
      c.dummy_read_bits  = 1;
      c.readable   = false;
      c.invert     = true;
      c.rgb_order  = false;
      c.dlen_16bit = false;
      c.bus_shared = false;
      _panel.config(c);
    }
    // Backlight PWM on GPIO 45
    {
      auto c = _light.config();
      c.pin_bl     = 45;
      c.invert     = false;
      c.freq       = 44100;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    // FT6336U capacitive touch — I2C on GPIO 5/6, interrupt on GPIO 7
    {
      auto c = _touch.config();
      c.x_min    = 0;
      c.x_max    = 479;
      c.y_min    = 0;
      c.y_max    = 319;
      c.pin_int  = 7;
      c.bus_shared = false;
      c.offset_rotation = 0;
      c.i2c_port = 0;
      c.i2c_addr = 0x38;
      c.pin_sda  = 6;
      c.pin_scl  = 5;
      c.freq     = 400000;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

// Defined in main.cpp; referenced here so all translation units can declare extern.
extern LGFX tft;
