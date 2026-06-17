#ifndef LZMA_H
#define LZMA_H

#include <stdint.h>
#include <stddef.h>

int lzma1_decode(uint8_t *dest, size_t *destLen,
                 const uint8_t *src, size_t srcLen,
                 const uint8_t *props, unsigned propsSize);

int vzip_decompress(const uint8_t *input, size_t input_size,
                    uint8_t **output, size_t *output_size);

#endif
