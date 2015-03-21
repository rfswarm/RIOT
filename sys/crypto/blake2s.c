/*
 * Copyright (C) 2015 Jan Wagner <mail@jwagner.eu>
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     sys_crypto
 * @{
 *
 * @file
 * @brief       Implementation of the BLAKE2s hash function
 *
 * @author      Jan Wagner <mail@jwagner.eu>
 * @author	Jean-Philippe Aumasson
 * @author	Samuel Neves
 * @author	Zooko Wilcox-O'Hearn
 * @author	Christian Winnerlein
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "crypto/blake2s.h"

/* BLAKE2s IVs */
static const uint32_t blake2s_IV[8] = {
    0x6A09E667UL, 0xBB67AE85UL, 0x3C6EF372UL, 0xA54FF53AUL,
    0x510E527FUL, 0x9B05688CUL, 0x1F83D9ABUL, 0x5BE0CD19UL
};

/* BLAKE2s sigma constants */
static const uint8_t blake2s_sigma[10][16] = {
    {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 } ,
    { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 } ,
    { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 } ,
    {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 } ,
    {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 } ,
    {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 } ,
    { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 } ,
    { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 } ,
    {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 } ,
    { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13 , 0 } ,
};

/* Load 32 bit value */
static inline uint32_t load32(const void *src)
{
    return *(uint32_t *)(src);
}

/* Store 32 bit value into dst */
static inline void store32(void *dst, uint32_t w)
{
    *(uint32_t *)(dst) = w;
}

/* Store 48 bit value into dst */
static inline void store48(void *dst, uint64_t w)
{
    uint8_t *p = (uint8_t *)dst;
    *p++ = (uint8_t)w;
    w >>= 8;
    *p++ = (uint8_t)w;
    w >>= 8;
    *p++ = (uint8_t)w;
    w >>= 8;
    *p++ = (uint8_t)w;
    w >>= 8;
    *p++ = (uint8_t)w;
    w >>= 8;
    *p++ = (uint8_t)w;
}

/* Rotate right - 32 bit */
static inline uint32_t rotr32(const uint32_t w, const unsigned c)
{
    return (w >> c) | (w << (32 - c));
}

/* Prevents compiler optimizing out memset() */
static inline void secure_zero_memory(void *v, size_t n)
{
    volatile uint8_t *p = (volatile uint8_t *)v;

    while (n--) {
        *p++ = 0;
    }
}

/* Set BLAKE2s state last node flag */
static inline int blake2s_set_lastnode(blake2s_state *S)
{
    S->f[1] = ~0U;
    return 0;
}

/* Clear BLAKE2s state last node flag */
static inline int blake2s_clear_lastnode(blake2s_state *S)
{
    S->f[1] = 0U;
    return 0;
}

/* Set BLAKE2s state last block flag */
static inline int blake2s_set_lastblock(blake2s_state *S)
{
    if (S->last_node) {
        blake2s_set_lastnode(S);
    }

    S->f[0] = ~0U;
    return 0;
}

/* Clear BLAKE2s state last block flag */
static inline int blake2s_clear_lastblock(blake2s_state *S)
{
    if (S->last_node) {
        blake2s_clear_lastnode(S);
    }

    S->f[0] = 0U;
    return 0;
}

/* Increment BLAKE2s state counter */
static inline int blake2s_increment_counter(blake2s_state *S, const uint32_t inc)
{
    S->t[0] += inc;
    S->t[1] += (S->t[0] < inc);
    return 0;
}

/* Set digest length parameter value */
static inline int blake2s_param_set_digest_length(blake2s_param *P, const uint8_t digest_length)
{
    P->digest_length = digest_length;
    return 0;
}

/* Set fanout parameter value */
static inline int blake2s_param_set_fanout(blake2s_param *P, const uint8_t fanout)
{
    P->fanout = fanout;
    return 0;
}

/* Set max depth parameter value */
static inline int blake2s_param_set_max_depth(blake2s_param *P, const uint8_t depth)
{
    P->depth = depth;
    return 0;
}

/* Set leaf length parameter value */
static inline int blake2s_param_set_leaf_length(blake2s_param *P, const uint32_t leaf_length)
{
    store32(&P->leaf_length, leaf_length);
    return 0;
}

/* Set node offset parameter value */
static inline int blake2s_param_set_node_offset(blake2s_param *P, const uint64_t node_offset)
{
    store48(P->node_offset, node_offset);
    return 0;
}

/* Set node depth parameter value */
static inline int blake2s_param_set_node_depth(blake2s_param *P, const uint8_t node_depth)
{
    P->node_depth = node_depth;
    return 0;
}

/* Set inner length parameter value */
static inline int blake2s_param_set_inner_length(blake2s_param *P, const uint8_t inner_length)
{
    P->inner_length = inner_length;
    return 0;
}

/* Set salt parameter value */
static inline int blake2s_param_set_salt(blake2s_param *P, const uint8_t salt[BLAKE2S_SALTBYTES])
{
    memcpy(P->salt, salt, BLAKE2S_SALTBYTES);
    return 0;
}

/* Set personal parameter value */
static inline int blake2s_param_set_personal(blake2s_param *P,
        const uint8_t personal[BLAKE2S_PERSONALBYTES])
{
    memcpy(P->personal, personal, BLAKE2S_PERSONALBYTES);
    return 0;
}

/* Initialize BLAKE2s state with IV constants */
static inline int blake2s_init0(blake2s_state *S)
{
    memset(S, 0, sizeof(blake2s_state));

    for (int i = 0; i < 8; ++i) {
        S->h[i] = blake2s_IV[i];
    }

    return 0;
}

/* Initialize xors IV with input parameter block */
int blake2s_init_param(blake2s_state *S, const blake2s_param *P)
{
    blake2s_init0(S);
    uint32_t *p = (uint32_t *)(P);

    /* IV XOR ParamBlock */
    for (size_t i = 0; i < 8; ++i) {
        S->h[i] ^= load32(&p[i]);
    }

    return 0;
}

/* Sequential BLAKE2s initialization */
int blake2s_init(blake2s_state *S, const uint8_t outlen)
{
    blake2s_param P[1];

    /* Move interval verification here? */
    if ((!outlen) || (outlen > BLAKE2S_OUTBYTES)) {
        return -1;
    }

    P->digest_length = outlen;
    P->key_length    = 0;
    P->fanout        = 1;
    P->depth         = 1;
    store32(&P->leaf_length, 0);
    store48(&P->node_offset, 0);
    P->node_depth    = 0;
    P->inner_length  = 0;

    memset(P->salt,     0, sizeof(P->salt));
    memset(P->personal, 0, sizeof(P->personal));
    return blake2s_init_param(S, P);
}

/* Initialize BLAKE2s state with key */
int blake2s_init_key(blake2s_state *S, const uint8_t outlen, const void *key, const uint8_t keylen)
{
    blake2s_param P[1];

    if ((!outlen) || (outlen > BLAKE2S_OUTBYTES)) {
        return -1;
    }

    if (!key || !keylen || keylen > BLAKE2S_KEYBYTES) {
        return -1;
    }

    P->digest_length = outlen;
    P->key_length    = keylen;
    P->fanout        = 1;
    P->depth         = 1;
    store32(&P->leaf_length, 0);
    store48(&P->node_offset, 0);
    P->node_depth    = 0;
    P->inner_length  = 0;

    memset(P->salt,     0, sizeof(P->salt));
    memset(P->personal, 0, sizeof(P->personal));

    if (blake2s_init_param(S, P) < 0) {
        return -1;
    }

    {
        uint8_t block[BLAKE2S_BLOCKBYTES];
        memset(block, 0, BLAKE2S_BLOCKBYTES);
        memcpy(block, key, keylen);
        blake2s_update(S, block, BLAKE2S_BLOCKBYTES);

        /* Burn the key from stack */
        secure_zero_memory(block, BLAKE2S_BLOCKBYTES);
    }

    return 0;
}

/* Perform BLAKE2s block compression */
static int blake2s_compress(blake2s_state *S, const uint8_t block[BLAKE2S_BLOCKBYTES])
{
    uint32_t m[16];
    uint32_t v[16];

    for (size_t i = 0; i < 16; ++i) {
        m[i] = load32(block + i * sizeof(m[i]));
    }

    for (size_t i = 0; i < 8; ++i) {
        v[i] = S->h[i];
    }

    v[ 8] = blake2s_IV[0];
    v[ 9] = blake2s_IV[1];
    v[10] = blake2s_IV[2];
    v[11] = blake2s_IV[3];
    v[12] = S->t[0] ^ blake2s_IV[4];
    v[13] = S->t[1] ^ blake2s_IV[5];
    v[14] = S->f[0] ^ blake2s_IV[6];
    v[15] = S->f[1] ^ blake2s_IV[7];

    BLAKE2S_ROUND(0);
    BLAKE2S_ROUND(1);
    BLAKE2S_ROUND(2);
    BLAKE2S_ROUND(3);
    BLAKE2S_ROUND(4);
    BLAKE2S_ROUND(5);
    BLAKE2S_ROUND(6);
    BLAKE2S_ROUND(7);
    BLAKE2S_ROUND(8);
    BLAKE2S_ROUND(9);

    for (size_t i = 0; i < 8; ++i) {
        S->h[i] = S->h[i] ^ v[i] ^ v[i + 8];
    }

    return 0;
}

/* Add bytes by performing BLAKE2s block compression */
int blake2s_update(blake2s_state *S, const uint8_t *in, uint64_t inlen)
{
    while (inlen > 0) {
        size_t left = S->buflen;
        size_t fill = 2 * BLAKE2S_BLOCKBYTES - left;

        if (inlen > fill) {
            /* Fill buffer */
            memcpy(S->buf + left, in, fill);
            S->buflen += fill;
            blake2s_increment_counter(S, BLAKE2S_BLOCKBYTES);

            /* Compress buffer */
            blake2s_compress(S, S->buf);

            /* Shift buffer left */
            memcpy(S->buf, S->buf + BLAKE2S_BLOCKBYTES, BLAKE2S_BLOCKBYTES);
            S->buflen -= BLAKE2S_BLOCKBYTES;
            in += fill;
            inlen -= fill;
        }
        else {
            /* inlen <= fill */
            memcpy(S->buf + left, in, inlen);

            /* Be lazy, do not compress */
            S->buflen += inlen;
            in += inlen;
            inlen -= inlen;
        }
    }

    return 0;
}

/* Finalize BLAKE2s hash output */
int blake2s_final(blake2s_state *S, uint8_t *out, uint8_t outlen)
{
    uint8_t buffer[BLAKE2S_OUTBYTES];

    if (S->buflen > BLAKE2S_BLOCKBYTES) {
        blake2s_increment_counter(S, BLAKE2S_BLOCKBYTES);
        blake2s_compress(S, S->buf);
        S->buflen -= BLAKE2S_BLOCKBYTES;
        memcpy(S->buf, S->buf + BLAKE2S_BLOCKBYTES, S->buflen);
    }

    blake2s_increment_counter(S, (uint32_t)S->buflen);
    blake2s_set_lastblock(S);

    /* Padding */
    memset(S->buf + S->buflen, 0, 2 * BLAKE2S_BLOCKBYTES - S->buflen);
    blake2s_compress(S, S->buf);

    /* Output full hash to temp buffer */
    for (int i = 0; i < 8; ++i) {
        store32(buffer + sizeof(S->h[i]) * i, S->h[i]);
    }

    memcpy(out, buffer, outlen);
    return 0;
}

/* BLAKE2s hash function */
int blake2s(uint8_t *out, const void *in, const void *key, const uint8_t outlen,
            const uint64_t inlen, uint8_t keylen)
{
    blake2s_state S[1];

    /* Verify parameters */
    if (NULL == in) {
        return -1;
    }

    if (NULL == out) {
        return -1;
    }

    /* Fail here instead if keylen != 0 and key == NULL? */
    if (NULL == key) {
        keylen = 0;
    }

    if (keylen > 0) {
        if (blake2s_init_key(S, outlen, key, keylen) < 0) {
            return -1;
        }
    }
    else {
        if (blake2s_init(S, outlen) < 0) {
            return -1;
        }
    }

    blake2s_update(S, (uint8_t *)in, inlen);
    blake2s_final(S, out, outlen);
    return 0;
}
/** @} */
