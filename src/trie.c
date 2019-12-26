/*
 * Copyright (c) 2015 Mikey Austin <mikey@greyd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * @file   trie.c
 * @brief  Implements the interface for a radix-trie (base 2).
 * @author Mikey Austin
 * @date   2015
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "failures.h"
#include "trie.h"

/* Return the nth bit of the multi-byte array x. */
#define BIT(x, n) ((((x)[(n) / 8]) & (0x1 << (7 - (n) % 8))) != 0)

#define IS_LEAF(t) ((t)->kids[0] == NULL && (t)->kids[1] == NULL)

static int bytecmp(const void*, int, const void*, int);

extern struct Trie* Trie_create(const unsigned char* key, int klen,
    int (*cmp)(const void*, int, const void*, int))
{
    struct Trie* trie;

    if ((trie = calloc(1, sizeof(*trie))) == NULL)
        i_critical("calloc: %s", strerror(errno));
    trie->cmp = cmp ? cmp : bytecmp;

    if (key) {
        trie->klen = klen;
        if ((trie->key = calloc(klen, sizeof(*trie->key))) == NULL)
            i_critical("calloc: %s", strerror(errno));
        memcpy(trie->key, key, klen);
    }

    return trie;
}

extern void
Trie_destroy(struct Trie* trie)
{
    int i;

    if (trie != NULL) {
        if (trie->key)
            free(trie->key);

        for (i = 0; i < TRIE_RADIX; i++)
            Trie_destroy(trie->kids[i]);

        free(trie);
    }
}

extern struct Trie* Trie_insert(struct Trie* trie, const unsigned char* key, int klen)
{
    struct Trie *t = trie, *kid;
    int kbits = klen * 8;

    if (trie == NULL) {
        return NULL;
    } else if (IS_LEAF(trie) && trie->key == NULL) {
        trie->kids[BIT(key, 0)] = Trie_create(key, klen, trie->cmp);
        return trie;
    }

    while (t && !IS_LEAF(t)) {
        if (t->branch >= kbits) {
            /* TODO: handle different key sizes. */
            break;
        } else {
            kid = t->kids[BIT(key, t->branch)];
            if (kid == NULL) {
                /* Insert new node here. */
                t->kids[BIT(key, t->branch)] = Trie_create(key, klen, trie->cmp);
                return trie;
            }
            t = kid;
        }
    }

    if (t && !trie->cmp(t->key, t->klen, key, klen)) {
        /* Already in trie. */
        return trie;
    }

    /* Split this node based on where the keys differ. */
    int bit = 0, val;
    while (bit < kbits && BIT(t->key, bit) == (val = BIT(key, bit))) {
        bit++;
    }

    t->branch = bit;
    t->kids[val] = Trie_create(key, klen, trie->cmp);
    t->kids[!val] = Trie_create(t->key, t->klen, trie->cmp);
    free(t->key);
    t->key = NULL;
    t->klen = 0;

    return trie;
}

extern int
Trie_contains(struct Trie* trie, const unsigned char* key, int klen)
{
    struct Trie* t = trie;
    int kbits = klen * 8;

    while (t && !IS_LEAF(t)) {
        if (t->branch >= kbits) {
            return 0;
        } else {
            t = t->kids[BIT(key, t->branch)];
        }
    }

    return t && !trie->cmp(t->key, t->klen, key, klen);
}

static int
bytecmp(const void* a, int alen, const void* b, int blen)
{
    const unsigned char* a_bytes = a;
    const unsigned char* b_bytes = b;

    if (alen != blen)
        return (alen > blen ? 1 : -1);

    return memcmp(a_bytes, b_bytes, alen);
}
