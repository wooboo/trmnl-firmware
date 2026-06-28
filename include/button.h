#include <stdint.h>

enum ButtonPressResult
{
  LongPress,
  DoubleClick,
  ShortPress,
  SoftReset,
  NoAction
};
extern const char *ButtonPressResultNames[5];

ButtonPressResult read_button_presses();

ButtonPressResult read_button_presses_on_pin(uint8_t pin);

ButtonPressResult read_long_press();
