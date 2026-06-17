#include "lzma.h"
#include "7zTypes.h"
#include "LzmaDec.h"
#include <stdlib.h>

static void *lzma_alloc_impl(void *p, size_t size) {
    (void)p;
    return malloc(size);
}
static void lzma_free_impl(void *p, void *addr) {
    (void)p;
    free(addr);
}
static ISzAlloc lzma_alloc = { lzma_alloc_impl, lzma_free_impl };

int lzma1_decode(uint8_t *dest, size_t *destLen,
                 const uint8_t *src, size_t srcLen,
                 const uint8_t *props, unsigned propsSize)
{
    if (propsSize < LZMA_PROPS_SIZE) return SZ_ERROR_UNSUPPORTED;
    if (srcLen < 5) return SZ_ERROR_INPUT_EOF;

    CLzmaDec dec;
    LzmaDec_Construct(&dec);

    SRes res = LzmaDec_Allocate(&dec, props, propsSize, &lzma_alloc);
    if (res != SZ_OK) return res;

    // Use the buffer interface
    SizeT outSize = *destLen;
    SizeT inSize = srcLen;
    ELzmaStatus status;
    res = LzmaDecode(dest, &outSize, src, &inSize,
                     props, propsSize, LZMA_FINISH_END,
                     &status, &lzma_alloc);

    if (res == SZ_OK) {
        *destLen = outSize;
    } else if (res == SZ_ERROR_INPUT_EOF && outSize > 0) {
        // Partial decompression may still be useful
        *destLen = outSize;
        res = SZ_OK;
    }

    LzmaDec_Free(&dec, &lzma_alloc);
    return res;
}

int vzip_decompress(const uint8_t *input, size_t input_size,
                    uint8_t **output, size_t *output_size)
{
    if (input_size < 17)
        return SZ_ERROR_DATA;
    if (input[0] != 'V' || input[1] != 'Z' || input[2] != 'a')
        return SZ_ERROR_DATA;

    uint8_t props = input[7];
    uint32_t dict_size = (uint32_t)input[8] | ((uint32_t)input[9] << 8) |
                         ((uint32_t)input[10] << 16) | ((uint32_t)input[11] << 24);

    size_t footer_off = input_size - 10;
    uint32_t decompressed_size = (uint32_t)input[footer_off + 4] |
                                 ((uint32_t)input[footer_off + 5] << 8) |
                                 ((uint32_t)input[footer_off + 6] << 16) |
                                 ((uint32_t)input[footer_off + 7] << 24);

    if (decompressed_size == 0 || decompressed_size > 256 * 1024 * 1024)
        return SZ_ERROR_DATA;

    uint8_t *decomp = (uint8_t *)malloc(decompressed_size);
    if (!decomp)
        return SZ_ERROR_MEM;

    // Build LZMA props byte + dict size (5 bytes)
    uint8_t lzma_props[5];
    lzma_props[0] = props;
    lzma_props[1] = (uint8_t)(dict_size);
    lzma_props[2] = (uint8_t)(dict_size >> 8);
    lzma_props[3] = (uint8_t)(dict_size >> 16);
    lzma_props[4] = (uint8_t)(dict_size >> 24);

    size_t lzma_src_len = input_size - 12 - 10;
    const uint8_t *lzma_src = input + 12;

    size_t dst_len = decompressed_size;
    int ret = lzma1_decode(decomp, &dst_len, lzma_src, lzma_src_len,
                            lzma_props, sizeof(lzma_props));

    if (ret != SZ_OK) {
        free(decomp);
        return ret;
    }

    *output = decomp;
    *output_size = decompressed_size;
    return SZ_OK;
}
