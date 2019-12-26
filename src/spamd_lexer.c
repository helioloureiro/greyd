/*
 * Copyright (c) 2014, 2015 Mikey Austin <mikey@greyd.org>
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
 * @file   spamd_lexer.c
 * @brief  Implements the spamd blacklist lexer.
 * @author Mikey Austin
 * @date   2014
 */

#include "spamd_lexer.h"
#include "failures.h"
#include "lexer_source.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

extern Lexer_T
Spamd_lexer_create(Lexer_source_T source)
{
    Lexer_T lexer;

    lexer = Lexer_create(source, Spamd_lexer_next_token);

    return lexer;
}

extern int
Spamd_lexer_next_token(Lexer_T lexer)
{
    int c, i, j;

    /*
     * Scan for the next token.
     */
    for (;;) {
        if (lexer->seen_end) {
            return SPAMD_LEXER_TOK_EOF;
        }

        c = L_GETC(lexer);

        if (isdigit(c)) {
            /*
             * This is either a 6 bit or 8 bit integer token. Strings of
             * digits greater than 255 will be split up into multiple
             * tokens, eg 25566 will yield 255 & 66 tokens.
             */

            i = (c - '0');
            while (isdigit(c = L_GETC(lexer))) {
                if ((j = (i * 10) + (c - '0')) > SPAMD_LEXER_MAX_INT8)
                    break;

                i = j;
            }

            lexer->current_value.i = i;
            Lexer_reuse_char(lexer, c);

            return lexer->current_token = (i <= SPAMD_LEXER_MAX_INT6 ? SPAMD_LEXER_TOK_INT6 : SPAMD_LEXER_TOK_INT8);
        }

        /*
         * Process remaining characters.
         */
        switch (c) {
        case ' ':
        case '\t':
        case '\r':
            /*
             * Ignore whitespace.
             */
            break;

        case '#':
            /*
             * Ignore everything until the end of the line (or end of file).
             */

            while ((c = L_GETC(lexer)) != '\n' && c != EOF)
                ;
            Lexer_reuse_char(lexer, c);

            continue;

        case '\n':
            return (lexer->current_token = SPAMD_LEXER_TOK_EOL);

        case '.':
            return (lexer->current_token = SPAMD_LEXER_TOK_DOT);

        case '-':
            return (lexer->current_token = SPAMD_LEXER_TOK_DASH);

        case '/':
            return (lexer->current_token = SPAMD_LEXER_TOK_SLASH);

        default:
            /* Unknown character. */
            return SPAMD_LEXER_TOK_EOF;
        }
    }
}
