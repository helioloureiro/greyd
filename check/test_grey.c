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
 * @file   test_grey.c
 * @brief  Unit tests for greylisting engine.
 * @author Mikey Austin
 * @date   2014
 */

#include "../src/config.h"

#include "test.h"
#include <config_lexer.h>
#include <config_parser.h>
#include <config_value.h>
#include <firewall.h>
#include <grey.h>
#include <greyd.h>
#include <greyd_config.h>
#include <greydb.h>
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

static void write_non_grey(int, char*, char*, long, FILE*);
static void write_grey(char*, char*, char*, char*, char*, FILE*);
static void write_trap(char* source, char* ip, long expires, FILE* grey_out);
static void write_white(char* source, char* ip, long expires, FILE* grey_out);
static void add_spamtrap(char* trapaddr, Config_T config);
static void tally_database(Config_T c, int* total_entries, int* total_white, int* total_grey,
    int* total_trapped, int* total_spamtrap, int* total_white_passed,
    int* total_white_blocked, int* total_grey_passed, int* total_grey_blocked);

int main(int argc, char* argv[])
{
    DB_handle_T db;
    struct DB_key key;
    struct DB_val val;
    List_T ips;
    struct List_entry* entry;
    Lexer_source_T ls, message_source;
    Lexer_T l, message_lexer;
    Config_parser_T cp, message_parser;
    Greylister_T greylister;
    Config_T c, message;
    Config_section_T section;
    int ret, grey[2], trap[2], fw[2], i;
    FILE *grey_in, *grey_out, *trap_out;
    pid_t reader_pid;
    char* domain;
    FW_handle_T fw_handle;
    char *conf, *conf_tmpl = "drop_privs = 0\n"
                             "low_prio_mx = \"192.179.21.3\"\n"
                             "section grey {\n"
                             "  db_permitted_domains = 1,\n"
                             "  permitted_domains = \"data/permitted_domains.txt\",\n"
                             "  traplist_name    = \"test traplist\",\n"
                             "  traplist_message = \"you have been trapped\",\n"
                             "  grey_expiry      = 3600\n"
                             "}\n"
                             "section firewall {\n"
                             "  driver = \"greyd_fw_dummy.so\"\n"
                             "}\n"
                             "section database {\n"
                             "  driver = \"%s\",\n"
                             "  path   = \"/tmp/greyd_test_grey\",\n"
                             "  db_name = \"test_grey_%s.db\"\n"
                             "}";

    TEST_START(34);

    asprintf(&conf, conf_tmpl, DB_DRIVER, DB_DRIVER);
    c = Config_create();
    if (argc > 1 && !strcmp(argv[1], "-")) {
        /* Read from stdin. */
        ls = Lexer_source_create_from_fd(0);
    } else {
        ls = Lexer_source_create_from_str(conf, strlen(conf));
    }
    l = Config_lexer_create(ls);
    cp = Config_parser_create(l);
    Config_parser_start(cp, c);

    /* Empty existing database file. */
    char* db_path;
    asprintf(&db_path, "%s/%s", Config_get_str(c, "path", "database", NULL),
        Config_get_str(c, "db_name", "database", NULL));
    ret = unlink(db_path);
    if (ret < 0 && errno != ENOENT) {
        printf("Error unlinking test DB %s: %s\n", db_path, strerror(errno));
    }
    free(db_path);

    add_spamtrap("trap@domain3.com", c);

    greylister = Grey_setup(c);
    TEST_OK((greylister != NULL), "Greylister created successfully");
    TEST_OK(!strcmp(greylister->traplist_name, "test traplist"),
        "greylister name set correctly");
    TEST_OK(!strcmp(greylister->traplist_msg, "you have been trapped"),
        "greylister msg set correctly");
    TEST_OK(!strcmp(greylister->low_prio_mx, "192.179.21.3"),
        "greylister low priority mx ip set correctly");
    TEST_OK(List_size(greylister->domains) == 2, "permitted domains size ok");

    /* Check the loaded domains. */
    i = 0;
    char* permitted_domains[2] = {
        "domain4.com",
        "domain2.com"
    };
    LIST_EACH(greylister->domains, entry)
    {
        domain = List_entry_value(entry);
        TEST_OK(!strcmp(permitted_domains[i++], domain), "domain loaded ok");
    }

    /* Load additional permitted domains into database. */
    DB_handle_T pdb;
    pdb = DB_init(c);
    DB_open(pdb, 0);
    memset(&key, 0, sizeof(key));
    memset(&val, 0, sizeof(val));
    key.type = DB_KEY_DOM;
    key.data.s = "greyd@domain3.com";
    DB_put(pdb, &key, &val);
    key.data.s = "domain1.com";
    DB_put(pdb, &key, &val);
    DB_close(&pdb);

    pipe(grey);
    grey_out = fdopen(grey[1], "w");
    grey_in = fdopen(grey[0], "r");

    /* Simulate a startup time in the past. */
    greylister->startup = time(NULL) - 120;
    greylister->grey_in = grey_in;
    greylister->fw_out = NULL;

    /* Start the grey reader child process. */
    switch (reader_pid = fork()) {
    case -1:
        goto cleanup;
        break;

    case 0:
        /* In child. */
        greylister->db_handle = DB_init(c);
        Grey_start_reader(greylister);
        fclose(grey_out);
        close(grey[1]);
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        free(__plan);
        exit(0);
        break;
    }

    /* In parent. */
    fclose(grey_in);
    close(grey[0]);
    greylister->grey_in = NULL;
    greylister->db_handle = DB_init(c);

    /* Send the reader entries over the pipe. */

    /* Write some grey entries. */
    write_grey("2.3.4.5", "1.2.3.4", "jackiemclean.net", "m@jackiemclean.net", "r@domain1.com", grey_out);
    write_grey("2.3.1.5", "1.2.4.4", "jackiemclean.net", "m@jackiemclean.net", "r@domain1.com", grey_out);
    write_grey("2.3.2.5", "1.2.2.4", "jackiemclean.net", "m@jackiemclean.net", "r@domain1.com", grey_out);

    /* Write the duplicate entries. */
    write_grey("2.3.4.5", "1.2.3.4", "jackiemclean.net", "m@jackiemclean.net", "r@domain1.com", grey_out);
    write_grey("2.3.1.5", "1.2.4.4", "jackiemclean.net", "m@jackiemclean.net", "r@domain1.com", grey_out);
    write_grey("2.3.2.5", "1.2.2.4", "jackiemclean.net", "m@jackiemclean.net", "r@domain1.com", grey_out);

    /* Write some white entries. */
    write_white("2.3.4.5", "4.3.2.1", time(NULL) + 3600, grey_out);
    write_white("2.3.4.6", "4.3.2.2", time(NULL) + 3600, grey_out);
    write_white("2.3.4.7", "4.3.2.3", time(NULL) + 3600, grey_out);

    /* Write duplicate white entries. */
    write_white("2.3.4.5", "4.3.2.1", time(NULL) + 3600, grey_out);
    write_white("2.3.4.6", "4.3.2.2", time(NULL) + 3600, grey_out);
    write_white("2.3.4.7", "4.3.2.3", time(NULL) + 3600, grey_out);

    /* Write some trap entries. */
    write_trap("3.2.4.5", "3.4.2.1", time(NULL) + 3600, grey_out);
    write_trap("3.2.4.6", "3.4.2.2", time(NULL) + 3600, grey_out);
    write_trap("3.2.4.7", "3.4.3.2", time(NULL) + 3600, grey_out);

    /* Write some duplicate trap entries. */
    write_trap("3.2.4.5", "3.4.2.1", time(NULL) + 3600, grey_out);
    write_trap("3.2.4.6", "3.4.2.2", time(NULL) + 3600, grey_out);
    write_trap("3.2.4.7", "3.4.3.2", time(NULL) + 3600, grey_out);

    /* Write an expired white & greytrap entry. */
    write_white("8.8.8.3", "7.7.6.5", time(NULL) - 3600, grey_out);
    write_trap("8.8.8.5", "7.7.6.6", time(NULL) - 120, grey_out);

    /* Entry to an explicit trap address in a permitted domain, will be trapped. */
    write_grey("2.3.2.5", "1.2.2.4", "jackiemclean.net", "m@jackiemclean.net",
        "trap@domain3.com", grey_out);

    /* This entry is to a domain not in the allowed list of domains, will be trapped. */
    write_grey("2.3.2.5", "1.2.2.4", "jackiemclean.net", "m@jackiemclean.net",
        "trap@willbetrapped.com", grey_out);

    /* Add a whitelist entry with the same ip as an existing grey entry. */
    write_white("2.3.4.7", "1.2.3.4", time(NULL) + 3600, grey_out);

    /* Trigger a hit to the low-priority MX server. */
    write_grey("192.179.21.3", "1.2.2.34", "jackiemclean.net", "m@jackiemclean.net",
        "notrap@domain4.com", grey_out);

    /* Forcing a parse error will kill the reader process. */
    fprintf(grey_out, "==\n");
    fclose(grey_out);
    close(grey[1]);
    waitpid(reader_pid, &ret, 0);

    /*
     * Test the overall database to ensue that the reader process correctly managed
     * the different types of entries.
     */
    int total_entries, total_grey, total_white, total_trapped, total_spamtrap;
    int total_white_passed, total_white_blocked;
    int total_grey_passed, total_grey_blocked;
    tally_database(c, &total_entries, &total_white, &total_grey, &total_trapped, &total_spamtrap,
        &total_white_passed, &total_white_blocked, &total_grey_passed, &total_grey_blocked);

    TEST_OK(total_entries == 17, "Total entries as expected");
    TEST_OK(total_white == 5, "Total white as expected");
    TEST_OK(total_grey == 3, "Total grey as expected");
    TEST_OK(total_trapped == 6, "Total trapped entries as expected");
    TEST_OK(total_spamtrap == 1, "Total spamtraps as expected");
    TEST_OK(total_white_passed == 3, "Total white passed as expected");
    TEST_OK(total_white_blocked == 0, "Total white blocked as expected");
    TEST_OK(total_grey_passed == 0, "Total grey passed as expected");
    TEST_OK(total_grey_blocked == 6, "Total grey blocked as expected");

    /*
     * Update some grey entry expiry times to simulate different conditions.
     */
    key.type = DB_KEY_TUPLE;
    key.data.gt.ip = "1.2.2.4";
    key.data.gt.from = "m@jackiemclean.net";
    key.data.gt.to = "r@domain1.com";
    key.data.gt.helo = "jackiemclean.net";

    db = DB_init(c);
    DB_open(db, 0);
    if (DB_get(db, &key, &val) == GREYDB_FOUND) {
        /* Update the expires time. */
        val.data.gd.expire = time(NULL) - 120;
        DB_put(db, &key, &val);
    }

    key.data.gt.ip = "1.2.4.4";
    if (DB_get(db, &key, &val) == GREYDB_FOUND) {
        /* Update the pass time. */
        val.data.gd.pass = time(NULL) - 60;
        DB_put(db, &key, &val);
    }
    DB_close(&db);

    /*
     * Setup dummy fw pipe.
     */
    fw_handle = FW_open(c);
    pipe(fw);
    greylister->fw_out = fdopen(fw[1], "w");

    /*
     * Test scan db routine for the sending of traplist entries over the
     * trap pipe.
     */
    pipe(trap);
    trap_out = fdopen(trap[1], "w");
    greylister->trap_out = trap_out;

    if (Grey_scan_db(greylister) < 0)
        return 1;

    /* Check the whitelist. */
    message_source = Lexer_source_create_from_fd(fw[0]);
    message_lexer = Config_lexer_create(message_source);
    message_parser = Config_parser_create(message_lexer);
    message = Config_create();

    ret = Config_parser_start(message_parser, message);
    TEST_OK((ret == CONFIG_PARSER_OK), "Parsed whitelist message");
    TEST_OK(!strcmp(Config_get_str(message, "name", NULL, ""), "greyd-whitelist"),
        "whitelist name ok");
    TEST_OK((Config_get_int(message, "af", NULL, 0) == AF_INET),
        "address family name ok");

    Greyd_process_fw_message(message, fw_handle, NULL);
    FW_close(&fw_handle);
    Config_destroy(&message);
    Config_parser_destroy(&message_parser);

    /* Check the traplist. */
    message_source = Lexer_source_create_from_fd(trap[0]);
    message_lexer = Config_lexer_create(message_source);
    message_parser = Config_parser_create(message_lexer);
    message = Config_create();

    ret = Config_parser_start(message_parser, message);
    TEST_OK(ret == CONFIG_PARSER_OK, "Parsed traplist message");

    section = Config_get_section(message, CONFIG_DEFAULT_SECTION);
    TEST_OK(!strcmp(Config_section_get_str(section, "name", "_"),
                "test traplist"),
        "Traplist name received ok");

    TEST_OK(!strcmp(Config_section_get_str(section, "message", "_"),
                "you have been trapped"),
        "Traplist message received ok");

    ips = Config_section_get_list(section, "ips");
    TEST_OK(ips != NULL, "Received traplist entries list exists");
    TEST_OK(List_size(ips) == 5, "ips list size is as expected");

    tally_database(c, &total_entries, &total_white, &total_grey, &total_trapped, &total_spamtrap,
        &total_white_passed, &total_white_blocked, &total_grey_passed, &total_grey_blocked);

    TEST_OK(total_entries == 14, "Total entries as expected");
    TEST_OK(total_white == 5, "Total white as expected");
    TEST_OK(total_grey == 1, "Total grey as expected");
    TEST_OK(total_trapped == 5, "Total trapped entries as expected");
    TEST_OK(total_spamtrap == 1, "Total spamtraps as expected");
    TEST_OK(total_white_passed == 3, "Total white passed as expected");
    TEST_OK(total_white_blocked == 2, "Total white blocked as expected");
    TEST_OK(total_grey_passed == 0, "Total grey passed as expected");
    TEST_OK(total_grey_blocked == 2, "Total grey blocked as expected");

cleanup:
    Grey_finish(&greylister);
    TEST_OK((greylister == NULL), "Greylister destroyed successfully");

    Config_destroy(&c);
    Config_destroy(&message);
    Config_parser_destroy(&cp);
    Config_parser_destroy(&message_parser);
    free(conf);

    TEST_COMPLETE;
}

/*
 * Test the overall database to ensue that the reader process correctly managed
 * the different types of entries.
 */
static void
tally_database(Config_T c, int* total_entries, int* total_white, int* total_grey,
    int* total_trapped, int* total_spamtrap, int* total_white_passed,
    int* total_white_blocked, int* total_grey_passed, int* total_grey_blocked)
{
    DB_handle_T db;
    DB_itr_T itr;
    struct DB_key key;
    struct DB_val val;

    *total_entries = *total_white = *total_grey = *total_trapped
        = *total_spamtrap = *total_white_passed = *total_white_blocked
        = *total_grey_passed = *total_grey_blocked = 0;

    db = DB_init(c);
    DB_open(db, 0);
    DB_start_txn(db);
    itr = DB_get_itr(db, DB_ENTRIES | DB_SPAMTRAPS | DB_DOMAINS);

    while (DB_itr_next(itr, &key, &val) != GREYDB_NOT_FOUND) {
        (*total_entries)++;

        switch (key.type) {
        case DB_KEY_IP:
            if (val.data.gd.pcount == -1) {
                (*total_trapped)++;
            } else {
                (*total_white)++;
                (*total_white_passed) += val.data.gd.pcount;
                (*total_white_blocked) += val.data.gd.bcount;
            }
            break;

        case DB_KEY_MAIL:
            (*total_spamtrap)++;
            break;

        case DB_KEY_TUPLE:
            (*total_grey)++;
            *total_grey_passed += val.data.gd.pcount;
            *total_grey_blocked += val.data.gd.bcount;
            break;
        }
    }

    DB_close_itr(&itr);
    DB_commit_txn(db);
    DB_close(&db);
}

static void
add_spamtrap(char* trapaddr, Config_T config)
{
    DB_handle_T db;
    struct DB_key key;
    struct DB_val val;
    struct Grey_data gd;

    key.type = DB_KEY_MAIL;
    key.data.s = trapaddr;

    memset(&gd, 0, sizeof(gd));
    gd.first = time(NULL);
    gd.bcount = 1;
    gd.expire = 0;
    gd.pcount = -2;

    val.type = DB_VAL_GREY;
    val.data.gd = gd;

    db = DB_init(config);
    DB_open(db, 0);
    DB_put(db, &key, &val);
    DB_close(&db);
}

static void
write_grey(char* dstip, char* ip, char* helo, char* from, char* to, FILE* grey_out)
{
    fprintf(grey_out,
        "type = %d\n"
        "dst_ip = \"%s\"\n"
        "ip = \"%s\"\n"
        "helo = \"%s\"\n"
        "from = \"%s\"\n"
        "to = \"%s\"\n"
        "%%\n",
        GREY_MSG_GREY, dstip, ip, helo, from, to);
    fflush(grey_out);
}

static void
write_non_grey(int type, char* source, char* ip, long expires, FILE* grey_out)
{
    fprintf(grey_out,
        "type = %d\n"
        "ip = \"%s\"\n"
        "source = \"%s\"\n"
        "expires = \"%ld\"\n"
        "%%\n",
        type, ip, source, expires);
    fflush(grey_out);
}

static void
write_white(char* source, char* ip, long expires, FILE* grey_out)
{
    write_non_grey(GREY_MSG_WHITE, source, ip, expires, grey_out);
}

static void
write_trap(char* source, char* ip, long expires, FILE* grey_out)
{
    write_non_grey(GREY_MSG_TRAP, source, ip, expires, grey_out);
}
