#include <cstdint>

enum bmp_err_e
{
  BMP_NO_ERR,
  BMP_NOT_BMP,
  BMP_BAD_SIZE,
  BMP_COLOR_SCHEME_FAILED,
  BMP_INVALID_OFFSET,
};

bmp_err_e parseBMPHeader(uint8_t *data, bool &reserved, int expectedWidth = 0, int expectedHeight = 0, int expectedBPP = 0, uint32_t dataSize = 0);
