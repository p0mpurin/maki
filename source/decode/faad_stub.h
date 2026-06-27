#ifndef FAAD_H
#define FAAD_H

#ifdef __cplusplus
extern "C" {
#endif

#define FAAD_FMT_16BIT 1
#define LC 2

typedef void *NeAACDecHandle;
typedef struct {
    unsigned long defObjectType;
    unsigned long outputFormat;
    unsigned long downMatrix;
    unsigned long useOldADTSFormat;
    unsigned long dontUpSampleImplicitSBR;
} NeAACDecConfiguration;

typedef NeAACDecConfiguration *NeAACDecConfigurationPtr;

typedef struct {
    unsigned long bytesconsumed;
    unsigned long channels;
    unsigned long error;
    unsigned long samplerate;
    unsigned long samples;
    unsigned long sbr;
    unsigned long header_type;
} NeAACDecFrameInfo;

NeAACDecHandle           NeAACDecOpen(void);
NeAACDecConfigurationPtr NeAACDecGetCurrentConfiguration(NeAACDecHandle hDecoder);
unsigned char            NeAACDecSetConfiguration(NeAACDecHandle hDecoder,
                                                   NeAACDecConfigurationPtr cfg);
long                     NeAACDecInit(NeAACDecHandle hDecoder,
                                      unsigned char *buffer, unsigned long buffer_size,
                                      unsigned long *samplerate, unsigned char *channels);
void *                   NeAACDecDecode(NeAACDecHandle hDecoder,
                                        NeAACDecFrameInfo *hInfo,
                                        unsigned char *buffer, unsigned long buffer_size);
void                     NeAACDecClose(NeAACDecHandle hDecoder);
char *                   NeAACDecGetErrorMessage(unsigned char errcode);

#ifdef __cplusplus
}
#endif

#endif
