#include "aes_xor.h"
#include <stdlib.h>
#include <string.h>
#include <mbedtls/aes.h>

static int base64_decode(const char *in, int in_len, unsigned char *out, int out_cap) {
    static const unsigned char table[128] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,62,0,0,0,63,52,53,54,55,56,57,58,59,60,61,0,0,0,0,0,
        0,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,0,0,0,0,0,
        0,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,0,0,0,0,0
    };

    int out_len = 0;
    int i = 0;
    unsigned char quad[4];

    for (i = 0; i < in_len; i += 4) {
        int j, pad = 0;
        for (j = 0; j < 4; j++) {
            if (i + j >= in_len) return out_len;
            if (in[i + j] == '=') { pad++; continue; }
            if (in[i + j] < 0 || in[i + j] > 127) return out_len;
            quad[j] = table[(unsigned char)in[i + j]];
        }

        if (out_len < out_cap) out[out_len++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
        if (pad < 2 && out_len < out_cap) out[out_len++] = (unsigned char)((quad[1] << 4) | (quad[2] >> 2));
        if (pad < 1 && out_len < out_cap) out[out_len++] = (unsigned char)((quad[2] << 6) | quad[3]);

        if (pad > 0) break;
    }
    return out_len;
}

char *decrypt_source_url(const char *obfuscated_url, const Config *cfg) {
    if (!obfuscated_url || !cfg) return NULL;

    int url_len = (int)strlen(obfuscated_url);
    int xor_key_len = (int)strlen(cfg->xor_key);
    if (xor_key_len == 0) return NULL;

    unsigned char *xor_result = (unsigned char *)malloc(url_len + 1);
    if (!xor_result) return NULL;

    int i;
    for (i = 0; i < url_len; i++) {
        xor_result[i] = (unsigned char)obfuscated_url[i] ^ (unsigned char)cfg->xor_key[i % xor_key_len];
    }
    xor_result[url_len] = 0;

    unsigned char *b64_out = (unsigned char *)malloc(url_len + 1);
    if (!b64_out) { free(xor_result); return NULL; }

    int b64_len = base64_decode((const char *)xor_result, url_len, b64_out, url_len);
    free(xor_result);

    if (b64_len <= 0) { free(b64_out); return NULL; }

    char *plaintext = (char *)malloc(b64_len + 1);
    if (!plaintext) { free(b64_out); return NULL; }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, cfg->aes_key, 256);

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    uint8_t iv_copy[16];
    memcpy(iv_copy, cfg->aes_iv, 16);

    mbedtls_aes_crypt_ctr(&aes, b64_len, &nc_off, iv_copy, stream_block,
                          b64_out, (unsigned char *)plaintext);
    mbedtls_aes_free(&aes);
    free(b64_out);

    plaintext[b64_len] = '\0';

    if (strncmp(plaintext, "http", 4) != 0) {
        free(plaintext);
        return NULL;
    }

    return plaintext;
}
