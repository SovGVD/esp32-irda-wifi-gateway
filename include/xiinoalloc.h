#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *xiino_decoder_malloc(size_t size);
void *xiino_decoder_calloc(size_t count,size_t size);
void xiino_decoder_free(void *pointer);
#ifdef __cplusplus
}
#endif
