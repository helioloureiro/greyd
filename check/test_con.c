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
 * @file   test_con.c
 * @brief  Unit tests for connection management module.
 * @author Mikey Austin
 * @date   2014
 */

#include "test.h"
#include <blacklist.h>
#include <con.h>
#include <config_lexer.h>
#include <config_parser.h>
#include <grey.h>
#include <greyd.h>
#include <greyd_config.h>
#include <hash.h>
#include <lexer.h>
#include <list.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    Lexer_source_T ls;
    Lexer_T l;
    Config_parser_T cp;
    Config_T c;
    struct Greyd_state gs;
    struct Con con;
    Blacklist_T bl1, bl2, bl3;
    struct sockaddr_storage src;
    int ret;
    char* conf = "hostname = \"greyd.org\"\n"
                 "banner   = \"greyd IP-based SPAM blocker\"\n"
                 "section grey {\n"
                 "  enable           = 1,\n"
                 "  traplist_name    = \"test traplist\",\n"
                 "  traplist_message = \"you have been trapped\",\n"
                 "  grey_expiry      = 3600,\n"
                 "  stutter          = 15\n"
                 "}\n"
                 "section firewall {\n"
                 "  driver = \"../drivers/fw_dummy.so\"\n"
                 "}\n"
                 "section database {\n"
                 "  driver = \"../drivers/bdb.so\",\n"
                 "  path   = \"/tmp/greyd_test_grey.db\"\n"
                 "}";

    /* Empty existing database file. */
    ret = unlink("/tmp/greyd_test_grey.db");
    if (ret < 0 && errno != ENOENT) {
        printf("Error unlinking test Berkeley DB: %s\n", strerror(errno));
    }

    TEST_START(49);

    c = Config_create();
    ls = Lexer_source_create_from_str(conf, strlen(conf));
    l = Config_lexer_create(ls);
    cp = Config_parser_create(l);
    Config_parser_start(cp, c);

    /*
     * Set up the greyd state and blacklists.
     */
    memset(&gs, 0, sizeof(gs));
    gs.config = c;
    gs.max_cons = 4;
    gs.max_black = 4;
    gs.blacklists = Hash_create(5, NULL);

    bl1 = Blacklist_create("blacklist_1", "You (%A) are on blacklist 1", BL_STORAGE_TRIE);
    bl2 = Blacklist_create("blacklist_2", "You (%A) are on blacklist 2", BL_STORAGE_TRIE);
    bl3 = Blacklist_create("blacklist_3_with_an_enormously_big_long_long_epic_epicly_long_large_name",
        "Your address %A\\nis on blacklist 3", BL_STORAGE_TRIE);

    Hash_insert(gs.blacklists, bl1->name, bl1);
    Hash_insert(gs.blacklists, bl2->name, bl2);
    Hash_insert(gs.blacklists, bl3->name, bl3);

    Blacklist_add(bl1, "10.10.10.1/32");
    Blacklist_add(bl1, "10.10.10.2/32");

    Blacklist_add(bl2, "10.10.10.1/32");
    Blacklist_add(bl2, "10.10.10.2/32");
    Blacklist_add(bl2, "2001::fad3:1/128");

    Blacklist_add(bl3, "10.10.10.2/32");
    Blacklist_add(bl3, "10.10.10.3/32");
    Blacklist_add(bl3, "2001::fad3:1/128");

    /*
     * Start testing the connection management.
     */
    memset(&src, 0, sizeof(src));
    ((struct sockaddr_in*)&src)->sin_family = AF_INET;
    inet_pton(AF_INET, "10.10.10.1", &((struct sockaddr_in*)&src)->sin_addr);

    memset(&con, 0, sizeof(con));
    Con_init(&con, 0, &src, &gs);

    TEST_OK(con.state == 0, "init state ok");
    TEST_OK(con.last_state == 0, "last state ok");
    TEST_OK(List_size(con.blacklists) == 2, "blacklist matches ok");
    TEST_OK(!strcmp(con.src_addr, "10.10.10.1"), "src addr ok");
    TEST_OK(con.out_buf != NULL, "out buf ok");
    TEST_OK(con.out_p == con.out_buf, "out buf pointer ok");
    TEST_OK(con.out_size == CON_OUT_BUF_SIZE, "out buf size ok");
    TEST_OK(!strcmp(con.lists, "blacklist_1 blacklist_2"),
        "list summary ok");

    /* The size of the banner. */
    TEST_OK(con.out_remaining == 75, "out buf remaining ok");

    TEST_OK(gs.clients == 1, "clients ok");
    TEST_OK(gs.black_clients == 1, "blacklisted clients ok");

    /*
     * Test the closing of a connection.
     */
    Con_close(&con, &gs);
    TEST_OK(List_size(con.blacklists) == 0, "blacklist empty ok");
    TEST_OK(con.out_buf == NULL, "out buf ok");
    TEST_OK(con.out_p == NULL, "out buf pointer ok");
    TEST_OK(con.out_size == 0, "out buf size ok");
    TEST_OK(con.lists == NULL, "lists ok");

    TEST_OK(gs.clients == 0, "clients ok");
    TEST_OK(gs.black_clients == 0, "blacklisted clients ok");

    /* Test recycling a connection. */
    memset(&src, 0, sizeof(src));
    ((struct sockaddr_in6*)&src)->sin6_family = AF_INET6;
    inet_pton(AF_INET6, "2001::fad3:1", &((struct sockaddr_in6*)&src)->sin6_addr);

    Con_init(&con, 0, &src, &gs);

    TEST_OK(con.state == 0, "init state ok");
    TEST_OK(con.last_state == 0, "last state ok");
    TEST_OK(List_size(con.blacklists) == 2, "blacklist matches ok");
    TEST_OK(!strcmp(con.src_addr, "2001::fad3:1"), "src addr ok");
    TEST_OK(con.out_buf != NULL, "out buf ok");
    TEST_OK(con.out_p == con.out_buf, "out buf pointer ok");
    TEST_OK(con.out_size == CON_OUT_BUF_SIZE, "out buf size ok");

    /*
     * As the 3rd blacklist's name is really long, the summarize lists
     * function should truncate with a "...".
     */
    TEST_OK(!strcmp(con.lists, "blacklist_2 ..."),
        "list summary ok");

    /* The size of the banner. */
    TEST_OK(con.out_remaining == 75, "out buf remaining ok");

    TEST_OK(gs.clients == 1, "clients ok");
    TEST_OK(gs.black_clients == 1, "blacklisted clients ok");

    /*
     * Test the rejection message building.
     */
    Con_build_reply(&con, "451");
    TEST_OK(!strcmp(con.out_p,
                "451-You (2001::fad3:1) are on blacklist 2\n"
                "451-Your address 2001::fad3:1\n"
                "451 is on blacklist 3\n"),
        "Blacklisted error response ok");
    TEST_OK(con.out_remaining == 94, "out buf remaining ok");

    /*
     * Test the writing of the buffer without stuttering.
     */
    time_t now = time(NULL);
    int con_pipe[2], to_write, nread;
    char in[CON_OUT_BUF_SIZE];

    pipe(con_pipe);
    con.fd = con_pipe[1];
    con.w = now;
    to_write = con.out_remaining;
    Con_handle_write(&con, &now, &gs);
    nread = read(con_pipe[0], in, to_write);
    in[nread] = '\0';

    TEST_OK(!strcmp(in,
                "451-You (2001::fad3:1) are on blacklist 2\n"
                "451-Your address 2001::fad3:1\n"
                "451 is on blacklist 3\n"),
        "Con write without stuttering ok");

    /*
     * Test the writing with stuttering. Note the reply is longer due
     * to the stutering adding in \r before each \n if there isn't one
     * already.
     */
    Con_build_reply(&con, "451");
    gs.max_cons = 100;
    gs.max_black = 100;
    con.w = now;
    while (con.out_remaining > 0) {
        Con_handle_write(&con, &now, &gs);
        now += con.stutter + 1;
    }

    memset(in, 0, sizeof(in));
    nread = read(con_pipe[0], in, to_write + 3);
    in[nread] = '\0';

    TEST_OK(!strcmp(in,
                "451-You (2001::fad3:1) are on blacklist 2\r\n"
                "451-Your address 2001::fad3:1\r\n"
                "451 is on blacklist 3\r\n"),
        "Con write with stuttering ok");

    /* Test recycling a connection, which is not on a blacklist. */
    Con_close(&con, &gs);
    memset(&src, 0, sizeof(src));
    ((struct sockaddr_in6*)&src)->sin6_family = AF_INET6;
    inet_pton(AF_INET6, "fa40::fad3:1", &((struct sockaddr_in6*)&src)->sin6_addr);

    Con_init(&con, 0, &src, &gs);
    TEST_OK(List_size(con.blacklists) == 0, "not on blacklist ok");

    /*
     * Note custom error codes only apply for blacklist connections, so expect
     * a 451 for this greylisted connections.
     */
    Con_build_reply(&con, "551");
    TEST_OK(!strcmp(con.out_p,
                "451 Temporary failure, please try again later.\r\n"),
        "greylisted error response ok");
    Con_close(&con, &gs);
    List_destroy(&con.blacklists);

    /*
     * Test the connection reading function.
     */
    memset(&src, 0, sizeof(src));
    ((struct sockaddr_in*)&src)->sin_family = AF_INET;
    inet_pton(AF_INET, "10.10.10.1", &((struct sockaddr_in*)&src)->sin_addr);

    pipe(con_pipe);
    memset(&con, 0, sizeof(con));
    Con_init(&con, con_pipe[0], &src, &gs);

    char* out = "EHLO greyd.org\r\n";
    write(con_pipe[1], out, strlen(out));

    Con_handle_read(&con, &now, &gs); /* This should change the state. */
    TEST_OK(con.state == CON_STATE_HELO_IN, "Initial state set ok");
    Con_handle_read(&con, &now, &gs);
    TEST_OK(!strcmp(con.in_buf, "EHLO greyd.org"), "con read ok");
    TEST_OK(!strcmp(con.helo, "greyd.org"), "helo parsed ok");
    TEST_OK(con.state = CON_STATE_HELO_OUT, "state helo out ok");

    char* mail_from = "MAIL FROM: <Mikey@greyd.ORG>\r\n";
    write(con_pipe[1], mail_from, strlen(mail_from));

    Con_next_state(&con, &now, &gs);
    TEST_OK(con.state = CON_STATE_MAIL_IN, "state mail in ok");
    Con_handle_read(&con, &now, &gs);
    TEST_OK(!strcmp(con.mail, "mikey@greyd.org"), "MAIL FROM parsed ok");
    TEST_OK(con.state = CON_STATE_MAIL_OUT, "state mail out ok");

    char* rcpt = "RCPT TO: info@greyd.org\r\n";
    write(con_pipe[1], rcpt, strlen(rcpt));

    Con_next_state(&con, &now, &gs);
    TEST_OK(con.state = CON_STATE_RCPT_IN, "state rcpt in ok");
    Con_handle_read(&con, &now, &gs);
    TEST_OK(!strcmp(con.rcpt, "info@greyd.org"), "RCPT parsed ok");
    TEST_OK(con.state = CON_STATE_RCPT_OUT, "state rcpt out ok");

    char* data = "DATA\r\n";
    write(con_pipe[1], data, strlen(data));

    Con_next_state(&con, &now, &gs);
    TEST_OK(con.state = CON_STATE_RCPT_IN, "state rcpt in ok"); /* this will goto spam. */
    Con_handle_read(&con, &now, &gs);
    TEST_OK(con.state = CON_STATE_DATA_OUT, "state data out ok");

    char* msg = "This is a spam message\r\ndeliver me!\r\n.\r\n";
    write(con_pipe[1], msg, strlen(msg));

    Con_next_state(&con, &now, &gs);
    TEST_OK(con.state = CON_STATE_MESSAGE, "state message ok");
    Con_handle_read(&con, &now, &gs);
    TEST_OK(con.state = CON_STATE_CLOSE, "state close ok");

    /* This should close the connection. */
    Con_next_state(&con, &now, &gs);

    /* Cleanup. */
    List_destroy(&con.blacklists);
    Hash_destroy(&gs.blacklists);
    Blacklist_destroy(&bl1);
    Blacklist_destroy(&bl2);
    Blacklist_destroy(&bl3);
    Config_destroy(&c);
    Config_parser_destroy(&cp);

    TEST_COMPLETE;
}
