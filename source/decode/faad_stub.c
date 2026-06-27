#include "faad_stub.h"
#include <stdlib.h>
#include <string.h>

NeAACDecHandle NeAACDecOpen(void) {
    return (NeAACDecHandle)malloc(1);
}

NeAACDecConfigurationPtr NeAACDecGetCurrentConfiguration(NeAACDecHandle hDecoder) {
    static NeAACDecConfiguration cfg;
    (void)hDecoder;
    memset(&cfg, 0, sizeof(cfg));
    return &cfg;
}

unsigned char NeAACDecSetConfiguration(NeAACDecHandle hDecoder,
                                        NeAACDecConfigurationPtr cfg) {
    (void)hDecoder;
    (void)cfg;
    return 1;
}

long NeAACDecInit(NeAACDecHandle hDecoder, unsigned char *buffer,
                   unsigned long buffer_size,
                   unsigned long *samplerate, unsigned char *channels) {
    (void)hDecoder;
    (void)buffer;
    (void)buffer_size;
    *samplerate = 44100;
    *channels = 2;
    return 0;
}

void * NeAACDecDecode(NeAACDecHandle hDecoder, NeAACDecFrameInfo *hInfo,
                       unsigned char *buffer, unsigned long buffer_size) {
    (void)hDecoder;
    (void)buffer;
    (void)buffer_size;
    memset(hInfo, 0, sizeof(NeAACDecFrameInfo));
    return NULL;
}

void NeAACDecClose(NeAACDecHandle hDecoder) {
    if (hDecoder) free(hDecoder);
}

char * NeAACDecGetErrorMessage(unsigned char errcode) {
    (void)errcode;
    return "stub";
}
