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
 * @file   greydb.h
 * @brief  Defines the generic greylisting database abstraction.
 * @author Mikey Austin
 * @date   2014
 */

#ifndef GREYDB_DEFINED
#define GREYDB_DEFINED

#include "grey.h"
#include "greyd_config.h"

#include <pwd.h>
#include <sys/types.h>
#include <time.h>

#define DB_KEY_IP 1 /**< An ip address. */
#define DB_KEY_MAIL 2 /**< A valid email address. */
#define DB_KEY_TUPLE 3 /**< A greylist tuple. */
#define DB_KEY_DOM 4 /**< A permitted domain. */
#define DB_KEY_DOM_PART 5 /**< A partial domain to match. */

#define DB_VAL_GREY 1 /**< Grey counters data. */

/*
 * Mutually exclusive bitmap to determine which results to
 * include in the iterator.
 */
#define DB_ENTRIES 1
#define DB_SPAMTRAPS 2
#define DB_DOMAINS 4

#define GREYDB_ERR -1
#define GREYDB_FOUND 0
#define GREYDB_OK 1
#define GREYDB_NOT_FOUND 2

#define GREYDB_RO 1
#define GREYDB_RW 2

/**
 * Keys may be of different types depending on the type of database entry.
 */
struct DB_key {
    short type;
    union {
        char* s; /**< May contain IP or MAIL key types. */
        struct Grey_tuple gt;
    } data;
};

struct DB_val {
    short type;
    union {
        struct Grey_data gd; /**< Greylisting counters.h */
    } data;
};

typedef struct DB_handle_T* DB_handle_T;
typedef struct DB_itr_T* DB_itr_T;

struct DB_handle_T {
    void* driver;
    void* dbh; /**< Driver dependent handle reference. */
    Config_T config; /**< System configuration. */
    struct passwd* pw; /**< System user/group information. */

    void (*db_init)(DB_handle_T handle);
    void (*db_open)(DB_handle_T handle, int);
    int (*db_start_txn)(DB_handle_T handle);
    int (*db_commit_txn)(DB_handle_T handle);
    int (*db_rollback_txn)(DB_handle_T handle);
    void (*db_close)(DB_handle_T handle);
    int (*db_put)(DB_handle_T handle, struct DB_key* key, struct DB_val* val);
    int (*db_get)(DB_handle_T handle, struct DB_key* key, struct DB_val* val);
    int (*db_del)(DB_handle_T handle, struct DB_key* key);
    void (*db_get_itr)(DB_itr_T itr, int types);
    int (*db_itr_next)(DB_itr_T itr, struct DB_key* key, struct DB_val* val);
    int (*db_itr_replace_curr)(DB_itr_T itr, struct DB_val* val);
    int (*db_itr_del_curr)(DB_itr_T itr);
    void (*db_itr_close)(DB_itr_T itr);
    int (*db_scan)(DB_handle_T handle, time_t* now, List_T whitelist,
        List_T whitelist_ipv6, List_T traplist, time_t* white_exp);
};

struct DB_itr_T {
    DB_handle_T handle; /**< Database handle reference. */
    void* dbi; /**< Driver specific iterator. */
    int current; /**< Current index. */
    unsigned long int size; /**< Number of elements in the iteration. */
};

/**
 * Initialize and return a database handle and configured database driver.
 */
extern DB_handle_T DB_init(Config_T config);

/**
 * Open a connection to the configured database based on the configuration.
 */
extern void DB_open(DB_handle_T handle, int flags);

/**
 * Start a new database transaction.
 */
extern int DB_start_txn(DB_handle_T handle);

/**
 * Commit a database transaction.
 */
extern int DB_commit_txn(DB_handle_T handle);

/**
 * Rollback a database transaction.
 */
extern int DB_rollback_txn(DB_handle_T handle);

/**
 * Close a database connection.
 */
extern void DB_close(DB_handle_T* handle);

/**
 * Insert a single key/value pair into the database.
 */
extern int DB_put(DB_handle_T handle, struct DB_key* key, struct DB_val* val);

/**
 * Get a single value out of the database, associated with the
 * specified key.
 */
extern int DB_get(DB_handle_T handle, struct DB_key* key, struct DB_val* val);

/**
 * Remove a record specified by the key.
 */
extern int DB_del(DB_handle_T handle, struct DB_key* key);

/**
 * Return an iterator for all database entries specified by the flag
 * *types*. If there are no entries, NULL is returned.
 */
extern DB_itr_T DB_get_itr(DB_handle_T handle, int types);

/**
 * Return the next key/value pair from the iterator.
 */
extern int DB_itr_next(DB_itr_T itr, struct DB_key* key, struct DB_val* val);

/**
 * Replace the value currently being pointed at by the iterator
 * with the supplied value.
 */
extern int DB_itr_replace_curr(DB_itr_T itr, struct DB_val* val);

/**
 * Delete the key/value pair currently being pointed at by
 * the iterator.
 */
extern int DB_itr_del_curr(DB_itr_T itr);

/**
 * Cleanup a used iterator.
 */
extern void DB_close_itr(DB_itr_T* itr);

/**
 * Scan the database and perform 3 main actions:
 *   - Delete expired entries
 *   - Populate the whitelist with IP (v4/v6) addresses
 *   - Populate the traplist with IP addresses
 */
extern int DB_scan(DB_handle_T handle, time_t* now, List_T whitelist,
    List_T whitelist_ipv6, List_T traplist,
    time_t* white_exp);

/**
 * Check the state of the supplied address via the opened
 * database handle.
 *
 * @return -1 on error
 * @return 0 when not found
 * @return 1 if greytrapped
 * @return 2 if whitelisted
 */
extern int DB_addr_state(DB_handle_T handle, char* addr);

#endif
