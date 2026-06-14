#include <bmp.h>
#include <trmnl_log.h>

/**
 * @brief Function to parse .bmp file header
 * @param data pointer to the buffer
 * @param reserved variable address to store parsed color schematic
 * @return bmp_err_e error code
 */
bmp_err_e parseBMPHeader(uint8_t *data, bool &reversed, int expectedWidth, int expectedHeight, int expectedBPP)
{

  // Check if the file is a BMP image
  if (data[0] != 'B' || data[1] != 'M')
  {
    Log_fatal("It is not a BMP file");
    return BMP_NOT_BMP;
  }
  // Get width and height from the header
  uint32_t width = *(uint32_t *)&data[18];
  uint32_t height = *(uint32_t *)&data[22];
  uint16_t bitsPerPixel = *(uint16_t *)&data[28];
  uint32_t compressionMethod = *(uint32_t *)&data[30];
  uint32_t imageDataSize = *(uint32_t *)&data[34];
  uint32_t colorTableEntries = *(uint32_t *)&data[46];
 
  if (colorTableEntries == 0) colorTableEntries = (1 << bitsPerPixel);

  // Validate dimensions if expected values are provided (0 = skip check)
  if (expectedWidth > 0 && (int)width != expectedWidth) {
    Log_fatal("BMP width mismatch: got %d, expected %d", (int)width, expectedWidth);
    return BMP_BAD_SIZE;
  }
  if (expectedHeight > 0 && (int)height != expectedHeight) {
    Log_fatal("BMP height mismatch: got %d, expected %d", (int)height, expectedHeight);
    return BMP_BAD_SIZE;
  }
  if (expectedBPP > 0 && (int)bitsPerPixel != expectedBPP) {
    Log_fatal("BMP bitsPerPixel mismatch: got %d, expected %d", (int)bitsPerPixel, expectedBPP);
    return BMP_BAD_SIZE;
  }

  // Fallback strict check for original TRMNL (backward compat when no params given)
  if (expectedWidth == 0 && expectedHeight == 0 && expectedBPP == 0) {
    if (width != 800 || height != 480 || bitsPerPixel != 1 || imageDataSize != 48000 || colorTableEntries != 2)
      return BMP_BAD_SIZE;
  }
  // Get the offset of the pixel data
  uint32_t dataOffset = *(uint32_t *)&data[10];

  // Display BMP information
  Log_info("BMP Header Information:\r\nWidth: %d\r\nHeight: %d\r\nBits per Pixel: %d\r\nCompression Method: %d\r\nImage Data Size: %d\r\nColor Table Entries: %d\r\nData offset: %d", width, height, bitsPerPixel, compressionMethod, imageDataSize, colorTableEntries, dataOffset);

  // Check if there's a color table
  if (dataOffset > 54)
  {
    // Read color table entries
    uint32_t colorTableSize = colorTableEntries * 4; // Each color entry is 4 bytes

    // Display color table
    Log_info("Color table");
    for (uint32_t i = 0; i < colorTableSize; i += 4)
    {
      Log_info("Color %d: B-%d, R-%d, G-%d, A-%d", i / 4 + 1, data[54 + i], data[55 + i], data[56 + i], data[57 + i]);
    }

    if (bitsPerPixel == 1) {
      // 1-bit BMP: check if color scheme is standard (black first) or reversed (white first)
      if (data[54] == 0 && data[55] == 0 && data[56] == 0 && data[57] == 0 && data[58] == 255 && data[59] == 255 && data[60] == 255 && data[61] == 0)
      {
        Log_info("Color scheme standart");
        reversed = false;
      }
      else if (data[54] == 255 && data[55] == 255 && data[56] == 255 && data[57] == 0 && data[58] == 0 && data[59] == 0 && data[60] == 0 && data[61] == 0)
      {
        Log_info("Color scheme reversed");
        reversed = true;
      }
      else
      {
        Log_info("Color scheme demaged");
        return BMP_COLOR_SCHEME_FAILED;
      }
    } else {
      // 4-bit (or higher) BMP — grayscale, color scheme check not applicable
      Log_info("Multi-bit BMP, skipping color scheme check");
    }
    return BMP_NO_ERR;
  }
  else
  {
    return BMP_INVALID_OFFSET;
  }
}