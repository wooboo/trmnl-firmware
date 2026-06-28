#include <filesystem.h>
#include <Arduino.h>
#include <trmnl_log.h>

#if defined(ARDUINO_ARCH_ESP32)
#include "esp_heap_caps.h"
#endif

#if defined(BOARD_X_CLASS) || defined(BOARD_M5STACK_PAPERCOLOR)
#include <LittleFS.h>
#define FS LittleFS
#else
#include <SPIFFS.h>
#define FS SPIFFS
#endif

static size_t filesystem_cache_identity_len(const char *path)
{
    if (!path)
    {
        return 0;
    }

    const char *underscore = strchr(path + 1, '_');
    if (underscore)
    {
        return static_cast<size_t>(underscore - path + 1);
    }

    size_t len = strlen(path);
    return len < 14 ? len : 14;
}

static bool filesystem_same_cache_identity(const char *a, const char *b)
{
    size_t a_len = filesystem_cache_identity_len(a);
    size_t b_len = filesystem_cache_identity_len(b);
    return a_len > 0 && a_len == b_len && strncmp(a, b, a_len) == 0;
}

/**
 * @brief Function to init the filesystem
 * @param none
 * @return bool result
 */
bool filesystem_init(void)
{
    if (!FS.begin(true))
    {
        Log_fatal("Failed to mount filesystem");
        ESP.restart();
        return false;
    }
    else
    {
        Log_info("Filesystem mounted");
        return true;
    }
}

/**
 * @brief Function to de-init the filesystem
 * @param none
 * @return none
 */
void filesystem_deinit(void)
{
    FS.end();
}

/**
 * @brief Function to read a file into a newly allocated buffer
 * @param name filename
 * @param out_buffer pointer to pointer of the output buffer
 * @return filesize in bytes or 0 if failed
 */
size_t filesystem_read_and_allocate(const char *name, uint8_t **out_buffer)
{
    uint8_t *buffer;
    size_t size;

    if (FS.exists(name)) {
        Log_info("file %s exists", name);
        File file = FS.open(name, FILE_READ);
        if (file) {
            size = file.size();
            if (size == 0) {
                Log_error("File %s is empty", name);
                file.close();
                return 0;
            }
#ifdef CONFIG_SPIRAM
            buffer = (uint8_t *)ps_malloc(size);
#else
            buffer = (uint8_t *)malloc(size);
#endif
            if (!buffer) {
                Log_error("Failed to allocate %d bytes", (int)size);
                file.close();
                return 0;
            }
            file.readBytes((char *)buffer, size);
            *out_buffer = buffer;
            file.close();
            return size;
        } else {
            Log_error("File %s open error", name);
            return 0;
        }
    } else {
        Log_info("file %s doesn\'t exist", name);
    }
    return 0;
} /* filesystem_read_and_allocate() */

/**
 * @brief Function to read data from file
 * @param name filename
 * @param out_buffer pointer to output buffer
 * @return result - true if success; false - if failed
 */
bool filesystem_read_from_file(const char *name, uint8_t *out_buffer, size_t size)
{
    if (FS.exists(name))
    {
        Log_info("file %s exists", name);
        File file = FS.open(name, FILE_READ);
        if (file)
        {
            file.readBytes((char *)out_buffer, size);
            return true;
        }
        else
        {
            Log_error("File %s open error", name);
            return false;
        }
    }
    else
    {
        Log_info("file %s doesn\'t exists", name);
        return false;
    }
}

/**
 * @brief Function to delete old versions of plugin images (by comparing the timestamp)
 *        It also deletes files that are older than 24h to keep SPIFFS from filling up
 * @param name filename
 * @return nothing
 */
void filesystem_purge_old_file(const char *name)
{
uint32_t u32;
time_t tt;
File rootDir; 
char *s, szTemp[32];
bool bDel;

    time(&tt); // get the current epoch time
    rootDir = FS.open("/");
    while (File file = rootDir.openNextFile()) {
        Log_info("Checking file \"%s\" for deletion", file.name());

        if (file.isDirectory()) {
            Log_info("Skipping directory \"%s\"", file.name());
            file.close();
            continue;
        }

        s = (char *)file.name();
        size_t fileNameLen = strlen(s);
        bool hasTimestamp = false;
        u32 = 0;
        if (fileNameLen >= 10)
        {
            const char *timestamp = &s[fileNameLen - 10];
            hasTimestamp = true;
            for (size_t i = 0; i < 10; ++i)
            {
                if (timestamp[i] < '0' || timestamp[i] > '9')
                {
                    hasTimestamp = false;
                    break;
                }
            }
            if (hasTimestamp)
            {
                u32 = (uint32_t)atoi(timestamp);
            }
        }
        bDel = false;

        snprintf(szTemp, sizeof(szTemp), "/%s", file.name());

        Log_info("Comparing name %s with %s, timestamp %u, current time %u", name, file.name(), u32, (uint32_t)tt);
        if (strcmp(name, szTemp) != 0 && filesystem_same_cache_identity(name, szTemp)) { // older version of the same file
            Log_info("Deleting older version of plugin image %s - %s", name, file.name());
            bDel = true;
        } else if (hasTimestamp && (uint32_t)tt > u32 && (uint32_t)tt - u32 > 60*60*24) { // More than 24h old
            Log_info("Deleting image older than 24h - %s", file.name());
            bDel = true;
        }
        if (bDel) { // to avoid double code
            Log_info("Deleting file %s", szTemp);
            file.close();
            FS.remove(szTemp);
        }
    }
    rootDir.close();

} /* filesystem_purge_old_file() */

/**
 * @brief Function to write data to file
 * @param name filename
 * @param in_buffer pointer to input buffer
 * @param size size of the input buffer
 * @return size of written bytes
 */
size_t filesystem_write_to_file(const char *name, uint8_t *in_buffer, size_t size)
{
    if (!name || !in_buffer || size == 0)
    {
        Log_error("Invalid file write request");
        return 0;
    }

    uint32_t FS_freeBytes = (FS.totalBytes() - FS.usedBytes());
    Log_info("FS free space - %d, total -%d", FS_freeBytes, FS.totalBytes());
    if (FS.exists(name))
    {
        Log_info("file %s exists. Deleting...", name);
        if (FS.remove(name))
            Log_info("file %s deleted", name);
        else
            Log_info("file %s deleting failed", name);
    }
    else
    {
        Log_info("file %s doesn't exist.", name);
    }
//    delay(100);
    File file = FS.open(name, FILE_WRITE, true);
    if (file)
    {
#if defined(ARDUINO_ARCH_ESP32)
        constexpr size_t WRITE_CHUNK_SIZE = 4096;
        uint8_t *writeBuffer = static_cast<uint8_t *>(heap_caps_malloc(WRITE_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!writeBuffer)
        {
            Log_error("Failed to allocate internal FS write buffer");
            file.close();
            return 0;
        }
#else
        constexpr size_t WRITE_CHUNK_SIZE = 4096;
#endif
        // Write the buffer in chunks
        size_t bytesWritten = 0;
        while (bytesWritten < size)
        {

            size_t diff = size - bytesWritten;
            size_t chunkSize = _min(WRITE_CHUNK_SIZE, diff);
#if defined(ARDUINO_ARCH_ESP32)
            memcpy(writeBuffer, in_buffer + bytesWritten, chunkSize);
            size_t res = file.write(writeBuffer, chunkSize);
#else
            size_t res = file.write(in_buffer + bytesWritten, chunkSize);
#endif
            if (res != chunkSize)
            {
                file.close();
#if defined(ARDUINO_ARCH_ESP32)
                free(writeBuffer);
#endif

                Log_info("Erasing FS...");
                if (FS.format())
                {
                    Log_info("FS erased successfully.");
                }
                else
                {
                    Log_error("Error erasing FS.");
                }

                return bytesWritten;
            }
            bytesWritten += chunkSize;
            yield();
        }
#if defined(ARDUINO_ARCH_ESP32)
        free(writeBuffer);
#endif
        Log_info("file %s writing success - %d bytes", name, bytesWritten);
        file.close();
        return bytesWritten;
    }
    else
    {
        Log_error("File open ERROR");
        return 0;
    }
}

/**
 * @brief Function to check if file exists
 * @param name filename
 * @return result - true if exists; false - if not exists
 */
bool filesystem_file_exists(const char *name)
{
    if (FS.exists(name))
    {
        Log_info("file %s exists.", name);
        return true;
    }
    else
    {
        Log_info("file %s does not exist.", name);
        return false;
    }
}

/**
 * @brief Function to delete the file
 * @param name filename
 * @return result - true if success; false - if failed
 */
bool filesystem_file_delete(const char *name)
{
    if (FS.exists(name))
    {
        if (FS.remove(name))
        {
            Log_info("file %s deleted", name);
            return true;
        }
        else
        {
            Log_error("file %s deleting failed", name);
            return false;
        }
    }
    else
    {
        Log_info("file %s doesn't exist", name);
        return true;
    }
}

/**
 * @brief Function to rename the file
 * @param old_name old filename
 * @param new_name new filename
 * @return result - true if success; false - if failed
 */
bool filesystem_file_rename(const char *old_name, const char *new_name)
{
    if (FS.exists(old_name))
    {
        Log_info("file %s exists.", old_name);
        bool res = FS.rename(old_name, new_name);
        if (res)
        {
            Log_info("file %s renamed to %s.", old_name, new_name);
            return true;
        }
        else
            Log_error("file %s wasn't renamed.", old_name);
        return false;
    }
    else
    {
        Log_info("file %s not exists.", old_name);
        return false;
    }
}

void list_files()
{
    Log_info("Filesystem Usage: %d/%d", FS.usedBytes(), FS.totalBytes());
    File rootDir = FS.open("/");

    while (File file = rootDir.openNextFile())
    {
        Log_info("  %d  %s", file.size(), file.name());
    }
    rootDir.close();
}
