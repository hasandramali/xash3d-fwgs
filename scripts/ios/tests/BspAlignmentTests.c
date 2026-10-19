#include "../../../engine/common/mod_bmodel.c"
#include <assert.h>

extern const unsigned char g_bootmap_xashnull[];
extern const unsigned int g_bootmap_xashnull_len;

static int live_allocations;

void Con_Printf(const char *format, ...) { (void)format; }
void Con_DPrintf(const char *format, ...) { (void)format; }
void Con_Reportf(const char *format, ...) { (void)format; }

void Sys_Error(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    abort();
}

void DBG_AssertFunction(qboolean expression, const char *text, const char *file, int line, const char *message) {
    (void)file;
    (void)line;
    (void)message;
    if (!expression) {
        fprintf(stderr, "assert: %s\n", text);
        abort();
    }
}

void *_Mem_Alloc(poolhandle_t pool, size_t size, qboolean clear, const char *file, int line) {
    (void)file;
    (void)line;
    assert(pool == 1);
    void *memory = clear ? calloc(1, size) : malloc(size);
    assert(memory);
    live_allocations++;
    return memory;
}

void _Mem_Free(void *memory, const char *file, int line) {
    (void)file;
    (void)line;
    assert(memory && live_allocations > 0);
    live_allocations--;
    free(memory);
}

int main(int argc, char **argv) {
    byte *buffer = malloc(g_bootmap_xashnull_len);
    assert(buffer);
    memcpy(buffer, g_bootmap_xashnull, g_bootmap_xashnull_len);
    dheader_t *header = (dheader_t *)buffer;
    assert(header->lumps[LUMP_PLANES].fileofs == 226);
    assert(header->lumps[LUMP_PLANES].fileofs % 4 == 2);

    model_t model = {0};
    model.mempool = 1;
    dbspmodel_t *bmod = Mem_Calloc(model.mempool, sizeof(*bmod));
    bmod->mempool = model.mempool;
    bmod->version = header->version;
    for (unsigned index = 0; index < ARRAYSIZE(srclumps); ++index) {
        const mlumpinfo_t *info = &srclumps[index];
        Mod_LoadLump(buffer, info, &worldstats[index], LUMP_SILENT, LOADLUMP_STANDARD, NULL, bmod);
        byte *data = *(byte **)((byte *)bmod + info->dataofs);
        if (data) {
            if (info->entrysize > sizeof(byte) || info->dataofs == offsetof(dbspmodel_t, textures))
                assert((uintptr_t)data % 4 == 0);
            const dlump_t *lump = &header->lumps[info->lumpnumber];
            assert(memcmp(data, buffer + lump->fileofs, lump->filelen) == 0);
        }
    }
    assert(bmod->numplanes == 47);
    if (argc > 1 && !strcmp(argv[1], "--unaligned"))
        bmod->planes = (dplane_t *)(buffer + header->lumps[LUMP_PLANES].fileofs);
    Mod_LoadPlanes(&model, bmod);
    for (int index = 0; index < model.numplanes; ++index) {
        dplane_t expected;
        memcpy(&expected, buffer + header->lumps[LUMP_PLANES].fileofs + index * sizeof(expected), sizeof(expected));
        for (int axis = 0; axis < 3; ++axis)
            assert(model.planes[index].normal[axis] == expected.normal[axis]);
        assert(model.planes[index].dist == expected.dist);
        assert(model.planes[index].type == expected.type);
    }
    Mod_FreeBmodelLumps(bmod);
    assert(live_allocations == 1);
    Mem_Free(model.planes);
    assert(memcmp(buffer, g_bootmap_xashnull, g_bootmap_xashnull_len) == 0);
    free(buffer);

    uint32_t packed_storage[128] = {0};
    byte *packed = (byte *)packed_storage;
    dheader_t *packed_header = (dheader_t *)packed;
    packed_header->version = HLBSP_VERSION;
    packed_header->lumps[LUMP_ENTITIES].fileofs = 125;
    packed_header->lumps[LUMP_ENTITIES].filelen = 32;
    packed_header->lumps[LUMP_TEXTURES].fileofs = 163;
    packed_header->lumps[LUMP_TEXTURES].filelen = 8;
    bmod = Mem_Calloc(model.mempool, sizeof(*bmod));
    bmod->mempool = model.mempool;
    for (unsigned index = 0; index < ARRAYSIZE(srclumps); ++index) {
        const mlumpinfo_t *info = &srclumps[index];
        if (info->lumpnumber == LUMP_ENTITIES || info->lumpnumber == LUMP_TEXTURES)
            Mod_LoadLump(packed, info, &worldstats[index], LUMP_SILENT, LOADLUMP_STANDARD, NULL, bmod);
    }
    assert(bmod->entdata == packed + 125);
    assert((byte *)bmod->textures != packed + 163);
    assert((uintptr_t)bmod->textures % 4 == 0);
    assert(bmod->textures->nummiptex == 0);
    Mod_FreeBmodelLumps(bmod);
    assert(live_allocations == 0);

    for (unsigned offset = 0; offset < 4; ++offset) {
        uint32_t storage[20];
        memset(storage, 0xa5, sizeof(storage));
        byte *input = (byte *)storage + offset;
        bmod = Mem_Calloc(model.mempool, sizeof(*bmod));
        bmod->mempool = model.mempool;
        byte *output = Mod_AlignLumpData(bmod, input, 64);
        assert((uintptr_t)output % 4 == 0);
        assert(memcmp(input, output, 64) == 0);
        assert((output == input) == (offset == 0));
        Mod_FreeBmodelLumps(bmod);
        assert(live_allocations == 0);
    }
    puts("PASS: embedded BSP lump alignment, plane decoding, zero-copy aligned input and temporary allocation cleanup");
    return 0;
}
