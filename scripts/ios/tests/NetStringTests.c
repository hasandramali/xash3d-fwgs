#include "common.h"
#include "net_buffer.h"
#include <assert.h>

void Con_Printf(const char *format, ...) { (void)format; }

void Sys_Error(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    abort();
}

void DBG_AssertFunction(qboolean expression, const char *text, const char *file, int line, const char *message) {
    (void)text;
    (void)file;
    (void)line;
    (void)message;
    assert(expression);
}

int main(int argc, char **argv) {
    byte data[32768] = {0};
    char output[8192];
    sizebuf_t message;
    const size_t mapcycleLength = 6023;
    memset(data, 'a', mapcycleLength);
    memcpy(data + 4095, "adat", 4);
    data[mapcycleLength] = 0;
    data[mapcycleLength + 1] = 0;
    data[mapcycleLength + 2] = 54;
    data[mapcycleLength + 3] = 0;
    data[mapcycleLength + 4] = 0;
    data[mapcycleLength + 5] = 14;

    MSG_Init(&message, "long-mapcycle", data, mapcycleLength + 6);
    assert(strlen(MSG_ReadString(&message)) == 4095);
    assert(MSG_ReadByte(&message) == 0);
    assert(MSG_ReadByte(&message) == 54);
    assert(!strcmp(MSG_ReadString(&message), ""));
    assert(MSG_ReadByte(&message) == 0);
    assert(MSG_ReadByte(&message) == 14);
    assert(!MSG_CheckOverflow(&message));

    MSG_Init(&message, "full-mapcycle", data, mapcycleLength + 6);
    assert(MSG_ReadStringBuffer(&message, output, sizeof(output)) == mapcycleLength);
    assert(!memcmp(output, data, mapcycleLength + 1));
    assert(MSG_ReadByte(&message) == 0 && MSG_ReadByte(&message) == 54);

    data[mapcycleLength] = '\n';
    MSG_Init(&message, "long-line", data, mapcycleLength + 6);
    assert(strlen(MSG_ReadStringLine(&message)) == 4095);
    assert(MSG_ReadByte(&message) == 0 && MSG_ReadByte(&message) == 54);

    memcpy(data, "100%\0\x36", 6);
    MSG_Init(&message, "format", data, 6);
    assert(MSG_ReadStringBuffer(&message, output, sizeof(output)) == 4);
    assert(!strcmp(output, "100."));
    assert(MSG_ReadByte(&message) == 54);
    MSG_Init(&message, "one-byte-output", data, 6);
    assert(MSG_ReadStringBuffer(&message, output, 1) == 0 && output[0] == 0);
    assert(MSG_ReadByte(&message) == 54);
    MSG_Init(&message, "discard", data, 6);
    assert(MSG_ReadStringBuffer(&message, NULL, 0) == 0);
    assert(MSG_ReadByte(&message) == 54);

    MSG_Init(&message, "unterminated", data, 4);
    assert(MSG_ReadStringBuffer(&message, output, sizeof(output)) == 4);
    assert(MSG_CheckOverflow(&message) && MSG_GetNumBitsLeft(&message) == 0);
    MSG_Init(&message, "empty-packet", data, 0);
    assert(MSG_ReadStringBuffer(&message, output, sizeof(output)) == 0);
    assert(MSG_CheckOverflow(&message));

    memset(data, 0, sizeof(data));
    MSG_Init(&message, "bit-packed", data, sizeof(data));
    MSG_WriteUBitLong(&message, 5, 3);
    MSG_WriteString(&message, "test");
    MSG_WriteByte(&message, 54);
    MSG_Init(&message, "bit-packed", data, 7);
    assert(MSG_ReadUBitLong(&message, 3) == 5);
    assert(!strcmp(MSG_ReadString(&message), "test"));
    assert(MSG_ReadByte(&message) == 54 && !MSG_CheckOverflow(&message));
    MSG_Init(&message, "partial-byte", data, 1);
    MSG_ReadUBitLong(&message, 3);
    assert(MSG_ReadStringBuffer(&message, output, sizeof(output)) == 0);
    assert(MSG_CheckOverflow(&message) && MSG_GetNumBitsLeft(&message) == 0);

    if (argc > 1) {
        FILE *file = fopen(argv[1], "rb");
        assert(file);
        memset(data, 0, sizeof(data));
        size_t length = fread(data, 1, sizeof(data) - 8, file);
        fclose(file);
        assert(length > 6210);
        MSG_Init(&message, "captured-serverdata", data + 12, length - 12);
        assert(MSG_SeekToBit(&message, 172 * 8, SEEK_SET) >= 0);
        assert(MSG_ReadStringBuffer(&message, output, sizeof(output)) == mapcycleLength);
        assert(MSG_GetNumBytesRead(&message) == 6196);
        assert(MSG_ReadByte(&message) == 0);
        assert(MSG_ReadByte(&message) == 54);
        assert(!strcmp(MSG_ReadString(&message), ""));
        assert(MSG_ReadByte(&message) == 0);
        assert(MSG_ReadByte(&message) == 14);
        assert(!strcmp(MSG_ReadString(&message), "event_t"));
        assert(!MSG_CheckOverflow(&message));
    }
    return 0;
}
