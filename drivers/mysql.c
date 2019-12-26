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
 * @file   mysql.c
 * @brief  MySQL DB driver.
 * @author Mikey Austin
 * @date   2015
 */

#include <config.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_MYSQL_H
#include <mysql.h>
#endif

#include "../src/failures.h"
#include "../src/greydb.h"
#include "../src/utils.h"

#define DEFAULT_HOST "localhost"
#define DEFAULT_PORT 3306
#define DEFAULT_DB "greyd"

/**
 * The internal driver handle.
 */
struct mysql_handle {
    MYSQL* db;
    char* greyd_host;
    int txn;
    int connected;
};

struct mysql_itr {
    MYSQL_RES* res;
    struct DB_key* curr;
};

static char* escape(MYSQL*, const char*);
static void populate_key(MYSQL_ROW, struct DB_key*, int);
static void populate_val(MYSQL_ROW, struct DB_val*, int);

extern void
Mod_db_init(DB_handle_T handle)
{
    struct mysql_handle* dbh;
    char *path, *hostname;
    int ret, flags, uid_changed = 0, len;

    if ((dbh = malloc(sizeof(*dbh))) == NULL)
        i_critical("malloc: %s", strerror(errno));

    handle->dbh = dbh;
    dbh->db = mysql_init(NULL);
    dbh->txn = 0;
    dbh->connected = 0;

    /* Escape and store the hostname in a static buffer. */
    hostname = Config_get_str(handle->config, "hostname", NULL, "");
    if ((dbh->greyd_host = calloc(2 * (len = strlen(hostname)) + 1,
             sizeof(char)))
        == NULL) {
        i_critical("malloc: %s", strerror(errno));
    }
    mysql_real_escape_string(dbh->db, dbh->greyd_host, hostname, len);
}

extern void
Mod_db_open(DB_handle_T handle, int flags)
{
    struct mysql_handle* dbh = handle->dbh;
    char *name, *host, *user, *pass, *socket, *sql;
    int port;

    if (dbh->connected)
        return;

    host = Config_get_str(handle->config, "host", "database", DEFAULT_HOST);
    port = Config_get_int(handle->config, "port", "database", DEFAULT_PORT);
    name = Config_get_str(handle->config, "name", "database", DEFAULT_DB);
    socket = Config_get_str(handle->config, "socket", "database", NULL);
    user = Config_get_str(handle->config, "user", "database", NULL);
    pass = Config_get_str(handle->config, "pass", "database", NULL);

    if (mysql_real_connect(dbh->db, host, user, pass,
            name, port, socket, 0)
        == NULL) {
        i_warning("could not connect to mysql %s:%d: %s",
            host, port,
            mysql_error(dbh->db));
        goto cleanup;
    }
    dbh->connected = 1;

    return;

cleanup:
    exit(1);
}

extern int
Mod_db_start_txn(DB_handle_T handle)
{
    struct mysql_handle* dbh = handle->dbh;
    int ret;

    if (dbh->txn != 0) {
        /* Already in a transaction. */
        return -1;
    }

    ret = mysql_query(dbh->db, "START TRANSACTION");
    if (ret != 0) {
        i_warning("start txn failed: %s", mysql_error(dbh->db));
        goto cleanup;
    }
    dbh->txn = 1;

    return ret;

cleanup:
    if (dbh->db)
        mysql_close(dbh->db);
    exit(1);
}

extern int
Mod_db_commit_txn(DB_handle_T handle)
{
    struct mysql_handle* dbh = handle->dbh;

    if (dbh->txn != 1) {
        i_warning("cannot commit, not in transaction");
        return -1;
    }

    if (mysql_commit(dbh->db)) {
        i_warning("db txn commit failed: %s", mysql_error(dbh->db));
        goto cleanup;
    }
    dbh->txn = 0;
    return 0;

cleanup:
    if (dbh->db)
        mysql_close(dbh->db);
    exit(1);
}

extern int
Mod_db_rollback_txn(DB_handle_T handle)
{
    struct mysql_handle* dbh = handle->dbh;

    if (dbh->txn != 1) {
        i_warning("cannot rollback, not in transaction");
        return -1;
    }

    if (mysql_rollback(dbh->db)) {
        i_warning("db txn rollback failed: %s", mysql_error(dbh->db));
        goto cleanup;
    }
    dbh->txn = 0;
    return 0;

cleanup:
    if (dbh->db)
        mysql_close(dbh->db);
    exit(1);
}

extern void
Mod_db_close(DB_handle_T handle)
{
    struct mysql_handle* dbh;

    if ((dbh = handle->dbh) != NULL) {
        free(dbh->greyd_host);
        if (dbh->db)
            mysql_close(dbh->db);
        free(dbh);
        handle->dbh = NULL;
    }
}

extern int
Mod_db_put(DB_handle_T handle, struct DB_key* key, struct DB_val* val)
{
    struct mysql_handle* dbh = handle->dbh;
    char *sql = NULL, *sql_tmpl = NULL;
    struct Grey_tuple* gt;
    struct Grey_data* gd;
    unsigned long len;
    char* add_esc = NULL;
    char* ip_esc = NULL;
    char* helo_esc = NULL;
    char* from_esc = NULL;
    char* to_esc = NULL;

    switch (key->type) {
    case DB_KEY_MAIL:
        sql_tmpl = "INSERT IGNORE INTO spamtraps(address) VALUES ('%s')";
        add_esc = escape(dbh->db, key->data.s);

        if (add_esc && asprintf(&sql, sql_tmpl, add_esc) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_DOM:
        sql_tmpl = "INSERT IGNORE INTO domains(domain) VALUES ('%s')";
        add_esc = escape(dbh->db, key->data.s);

        if (add_esc && asprintf(&sql, sql_tmpl, add_esc) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_IP:
        sql_tmpl = "REPLACE INTO entries "
                   "(`ip`, `helo`, `from`, `to`, "
                   " `first`, `pass`, `expire`, `bcount`, `pcount`, `greyd_host`) "
                   "VALUES "
                   "('%s', '', '', '', %lld, %lld, %lld, %d, %d, '%s')";
        add_esc = escape(dbh->db, key->data.s);

        gd = &val->data.gd;
        if (add_esc && asprintf(&sql, sql_tmpl, add_esc, gd->first, gd->pass, gd->expire, gd->bcount, gd->pcount, dbh->greyd_host) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_TUPLE:
        sql_tmpl = "REPLACE INTO entries "
                   "(`ip`, `helo`, `from`, `to`, "
                   " `first`, `pass`, `expire`, `bcount`, `pcount`, `greyd_host`) "
                   "VALUES "
                   "('%s', '%s', '%s', '%s', %lld, %lld, %lld, %d, %d, '%s')";
        gt = &key->data.gt;
        ip_esc = escape(dbh->db, gt->ip);
        helo_esc = escape(dbh->db, gt->helo);
        from_esc = escape(dbh->db, gt->from);
        to_esc = escape(dbh->db, gt->to);

        gd = &val->data.gd;
        if (ip_esc && helo_esc && from_esc && to_esc
            && asprintf(&sql, sql_tmpl, ip_esc, helo_esc, from_esc, to_esc,
                   gd->first, gd->pass, gd->expire, gd->bcount,
                   gd->pcount, dbh->greyd_host)
                == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(ip_esc);
        free(helo_esc);
        free(from_esc);
        free(to_esc);
        break;

    default:
        return GREYDB_ERR;
    }

    if (mysql_real_query(dbh->db, sql, strlen(sql)) != 0) {
        i_warning("put mysql error: %s", mysql_error(dbh->db));
        free(sql);
        goto err;
    }
    free(sql);
    return GREYDB_OK;

err:
    DB_rollback_txn(handle);
    return GREYDB_ERR;
}

extern int
Mod_db_get(DB_handle_T handle, struct DB_key* key, struct DB_val* val)
{
    struct mysql_handle* dbh = handle->dbh;
    char *sql = NULL, *sql_tmpl = NULL;
    struct Grey_tuple* gt;
    unsigned long len;
    int res = GREYDB_NOT_FOUND;
    char* add_esc = NULL;
    char* ip_esc = NULL;
    char* helo_esc = NULL;
    char* from_esc = NULL;
    char* to_esc = NULL;

    switch (key->type) {
    case DB_KEY_DOM_PART:
        sql_tmpl = "SELECT 0, 0, 0, 0, -3 "
                   "FROM domains WHERE '%s' LIKE CONCAT('%', domain)";
        add_esc = escape(dbh->db, key->data.s);

        if (add_esc && asprintf(&sql, sql_tmpl, add_esc) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_DOM:
        sql_tmpl = "SELECT 0, 0, 0, 0, -3 "
                   "FROM domains WHERE `domain`='%s' "
                   "LIMIT 1";
        add_esc = escape(dbh->db, key->data.s);

        if (add_esc && asprintf(&sql, sql_tmpl, add_esc) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_MAIL:
        sql_tmpl = "SELECT 0, 0, 0, 0, -2 "
                   "FROM spamtraps WHERE `address`='%s' "
                   "LIMIT 1";
        add_esc = escape(dbh->db, key->data.s);

        if (add_esc && asprintf(&sql, sql_tmpl, add_esc) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_IP:
        sql_tmpl = "SELECT `first`, `pass`, `expire`, `bcount`, `pcount` "
                   "FROM entries "
                   "WHERE `ip`='%s' AND `helo`='' AND `from`='' AND `to`='' "
                   "LIMIT 1";
        add_esc = escape(dbh->db, key->data.s);

        if (add_esc && asprintf(&sql, sql_tmpl, add_esc) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_TUPLE:
        sql_tmpl = "SELECT `first`, `pass`, `expire`, `bcount`, `pcount` "
                   "FROM entries "
                   "WHERE `ip`='%s' AND `helo`='%s' AND `from`='%s' AND `to`='%s' "
                   "LIMIT 1";
        gt = &key->data.gt;
        ip_esc = escape(dbh->db, gt->ip);
        helo_esc = escape(dbh->db, gt->helo);
        from_esc = escape(dbh->db, gt->from);
        to_esc = escape(dbh->db, gt->to);

        if (ip_esc && helo_esc && from_esc && to_esc
            && asprintf(&sql, sql_tmpl, ip_esc, helo_esc, from_esc,
                   to_esc)
                == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(ip_esc);
        free(helo_esc);
        free(from_esc);
        free(to_esc);
        break;

    default:
        return GREYDB_ERR;
    }

    MYSQL_RES* result = NULL;
    MYSQL_ROW row;
    MYSQL_FIELD* field;
    unsigned int expected_fields = 5;

    if (mysql_real_query(dbh->db, sql, strlen(sql)) != 0) {
        i_warning("get mysql error: %s", mysql_error(dbh->db));
        goto err;
    }

    if ((result = mysql_store_result(dbh->db)) != NULL
        && mysql_num_fields(result) == expected_fields
        && mysql_num_rows(result) == 1) {
        res = GREYDB_FOUND;
        row = mysql_fetch_row(result);
        populate_val(row, val, 0);
    }

err:
    if (sql != NULL)
        free(sql);
    if (result != NULL)
        mysql_free_result(result);
    return res;
}

extern int
Mod_db_del(DB_handle_T handle, struct DB_key* key)
{
    struct mysql_handle* dbh = handle->dbh;
    char *sql = NULL, *sql_tmpl = NULL;
    struct Grey_tuple* gt;
    unsigned long len;
    char* add_esc = NULL;
    char* ip_esc = NULL;
    char* helo_esc = NULL;
    char* from_esc = NULL;
    char* to_esc = NULL;

    switch (key->type) {
    case DB_KEY_MAIL:
        sql_tmpl = "DELETE FROM spamtraps WHERE `address`='%s'";
        add_esc = escape(dbh->db, key->data.s);

        if (add_esc && asprintf(&sql, sql_tmpl, add_esc) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_DOM:
        sql_tmpl = "DELETE FROM domains WHERE `domain`='%s'";
        add_esc = escape(dbh->db, key->data.s);

        if (add_esc && asprintf(&sql, sql_tmpl, add_esc) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_IP:
        sql_tmpl = "DELETE FROM entries WHERE `ip`='%s' "
                   "AND `helo`='' AND `from`='' AND `to`=''";
        add_esc = escape(dbh->db, key->data.s);

        if (add_esc && asprintf(&sql, sql_tmpl, add_esc) == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(add_esc);
        break;

    case DB_KEY_TUPLE:
        sql_tmpl = "DELETE FROM entries WHERE `ip`='%s' "
                   "AND `helo`='%s' AND `from`='%s' AND `to`='%s' ";
        gt = &key->data.gt;
        ip_esc = escape(dbh->db, gt->ip);
        helo_esc = escape(dbh->db, gt->helo);
        from_esc = escape(dbh->db, gt->from);
        to_esc = escape(dbh->db, gt->to);

        if (ip_esc && helo_esc && from_esc && to_esc
            && asprintf(&sql, sql_tmpl, ip_esc, helo_esc, from_esc,
                   to_esc)
                == -1) {
            i_warning("mysql asprintf error");
            goto err;
        }
        free(ip_esc);
        free(helo_esc);
        free(from_esc);
        free(to_esc);
        break;

    default:
        return GREYDB_ERR;
    }

    if (mysql_real_query(dbh->db, sql, strlen(sql)) != 0) {
        i_warning("del mysql error: %s", mysql_error(dbh->db));
        free(sql);
        goto err;
    }
    free(sql);
    return GREYDB_OK;

err:
    DB_rollback_txn(handle);
    return GREYDB_ERR;
}

extern void
Mod_db_get_itr(DB_itr_T itr, int types)
{
    struct mysql_handle* dbh = itr->handle->dbh;
    struct mysql_itr* dbi = NULL;
    char *sql_tmpl, *sql = NULL;

    if ((dbi = malloc(sizeof(*dbi))) == NULL) {
        i_warning("malloc: %s", strerror(errno));
        goto err;
    }
    dbi->res = NULL;
    dbi->curr = NULL;
    itr->dbi = dbi;

    sql_tmpl = "SELECT `ip`, `helo`, `from`, `to`, "
               "`first`, `pass`, `expire`, `bcount`, `pcount` FROM entries "
               "WHERE %d "
               "UNION "
               "SELECT `address`, '', '', '', 0, 0, 0, 0, -2 FROM spamtraps "
               "WHERE %d "
               "UNION "
               "SELECT `domain`, '', '', '', 0, 0, 0, 0, -3 FROM domains "
               "WHERE %d";
    if (asprintf(&sql, sql_tmpl, types & DB_ENTRIES, types & DB_SPAMTRAPS,
            types & DB_DOMAINS)
        == -1) {
        i_warning("asprintf");
        goto err;
    }

    if (sql && mysql_real_query(dbh->db, sql, strlen(sql)) != 0) {
        i_warning("insert mysql error: %s", mysql_error(dbh->db));
        free(sql);
        goto err;
    }
    free(sql);

    dbi->res = mysql_store_result(dbh->db);
    itr->size = mysql_num_rows(dbi->res);

    return;

err:
    DB_rollback_txn(itr->handle);
    exit(1);
}

extern void
Mod_db_itr_close(DB_itr_T itr)
{
    struct mysql_handle* dbh = itr->handle->dbh;
    struct mysql_itr* dbi = itr->dbi;

    if (dbi) {
        dbi->curr = NULL;
        if (dbi->res != NULL)
            mysql_free_result(dbi->res);
        free(dbi);
        itr->dbi = NULL;
    }
}

extern int
Mod_db_itr_next(DB_itr_T itr, struct DB_key* key, struct DB_val* val)
{
    struct mysql_itr* dbi = itr->dbi;
    MYSQL_ROW row;

    if (dbi->res && (row = mysql_fetch_row(dbi->res))) {
        populate_key(row, key, 0);
        populate_val(row, val, 4);
        dbi->curr = key;
        itr->current++;
        return GREYDB_FOUND;
    }

    return GREYDB_NOT_FOUND;
}

extern int
Mod_db_itr_replace_curr(DB_itr_T itr, struct DB_val* val)
{
    struct mysql_handle* dbh = itr->handle->dbh;
    struct mysql_itr* dbi = itr->dbi;
    struct DB_key* key = dbi->curr;

    return DB_put(itr->handle, key, val);
}

extern int
Mod_db_itr_del_curr(DB_itr_T itr)
{
    struct mysql_itr* dbi = itr->dbi;

    if (dbi && dbi->res && dbi->curr)
        return DB_del(itr->handle, dbi->curr);

    return GREYDB_ERR;
}

extern int
Mod_scan_db(DB_handle_T handle, time_t* now, List_T whitelist,
    List_T whitelist_ipv6, List_T traplist, time_t* white_exp)
{
    struct mysql_handle* dbh = handle->dbh;
    char *sql, *sql_tmpl;
    int ret = GREYDB_OK;

    /*
     * Delete expired entries and whitelist appropriate grey entries,
     * by un-setting the tuple fields (to, from, helo), but only if there
     * is not already a conflicting entry with the same IP address
     * (ie an existing trap entry).
     */
    sql_tmpl = "DELETE FROM entries WHERE expire <= UNIX_TIMESTAMP() "
               "AND `greyd_host`='%s'";

    if (asprintf(&sql, sql_tmpl, dbh->greyd_host) == -1) {
        i_warning("mysql asprintf error");
        goto err;
    }

    if (mysql_real_query(dbh->db, sql, strlen(sql)) != 0) {
        i_warning("delete mysql expired entries: %s", mysql_error(dbh->db));
        goto err;
    }
    free(sql);
    sql = NULL;

    sql_tmpl = "UPDATE IGNORE entries e LEFT JOIN entries g "
               "ON g.`ip`=e.`ip` AND g.`to`='' AND g.`from`='' "
               "SET e.`helo` = '', e.`from` = '', e.`to` = '', e.`expire` = %lld "
               "WHERE e.`from` <> '' AND e.`to` <> '' AND e.`pcount` >= 0 "
               "AND g.`ip` IS NULL AND e.`pass` <= UNIX_TIMESTAMP() "
               "AND e.`greyd_host`='%s'";

    if (asprintf(&sql, sql_tmpl, *now + *white_exp, dbh->greyd_host) == -1) {
        i_warning("mysql asprintf error");
        goto err;
    }

    if (mysql_real_query(dbh->db, sql, strlen(sql)) != 0) {
        i_warning("update mysql entries: %s", mysql_error(dbh->db));
        goto err;
    }
    free(sql);
    sql = NULL;

    /* Add greytrap & whitelist entries. */
    sql = "SELECT `ip`, NULL, NULL FROM entries             \
        WHERE `to`='' AND `from`='' AND `ip` NOT LIKE '%:%' \
            AND `pcount` >= 0                               \
        UNION                                               \
        SELECT NULL, `ip`, NULL FROM entries                \
        WHERE `to`='' AND `from`='' AND `ip` LIKE '%:%'     \
            AND `pcount` >= 0                               \
        UNION                                               \
        SELECT NULL, NULL, `ip` FROM entries                \
        WHERE `to`='' AND `from`='' AND `pcount` < 0";

    if (mysql_real_query(dbh->db, sql, strlen(sql)) != 0) {
        i_warning("mysql fetch grey/white entries: %s", mysql_error(dbh->db));
        goto err;
    }

    MYSQL_RES* result = mysql_store_result(dbh->db);
    MYSQL_ROW row;

    if (result && mysql_num_rows(result) > 0) {
        while ((row = mysql_fetch_row(result))) {
            if (row[0] != NULL) {
                List_insert_after(
                    whitelist, strdup((const char*)row[0]));
            } else if (row[1] != NULL) {
                List_insert_after(
                    whitelist_ipv6, strdup((const char*)row[1]));
            } else if (row[2] != NULL) {
                List_insert_after(
                    traplist, strdup((const char*)row[2]));
            }
        }
    }

err:
    if (result)
        mysql_free_result(result);
    return ret;
}

static char* escape(MYSQL* db, const char* str)
{
    char* esc;
    size_t len = strlen(str);

    if ((esc = calloc(2 * len + 1, sizeof(char))) == NULL) {
        i_warning("calloc: %s", strerror(errno));
        return NULL;
    }
    mysql_real_escape_string(db, esc, str, len);

    return esc;
}

static void
populate_key(MYSQL_ROW row, struct DB_key* key, int from)
{
    struct Grey_tuple* gt;
    static char buf[(INET6_ADDRSTRLEN + 1) + 3 * (GREY_MAX_MAIL + 1)];
    char* buf_p = buf;

    memset(key, 0, sizeof(*key));
    memset(buf, 0, sizeof(buf));

    /*
     * Empty helo, from & to columns indicate a non-grey entry.
     * A pcount of -2 indicates a spamtrap, and a -3 indicates
     * a permitted domain.
     */
    key->type = ((!memcmp(row[1], "", 1)
                     && !memcmp(row[2], "", 1)
                     && !memcmp(row[3], "", 1))
            ? (atoi(row[8]) == -2
                    ? DB_KEY_MAIL
                    : (atoi(row[8]) == -3
                            ? DB_KEY_DOM
                            : DB_KEY_IP))
            : DB_KEY_TUPLE);

    if (key->type == DB_KEY_IP) {
        key->data.s = buf_p;
        sstrncpy(buf_p, (const char*)row[from + 0], INET6_ADDRSTRLEN + 1);
    } else if (key->type == DB_KEY_MAIL || key->type == DB_KEY_DOM) {
        key->data.s = buf_p;
        sstrncpy(buf_p, (const char*)row[from + 0], GREY_MAX_MAIL + 1);
    } else {
        gt = &key->data.gt;
        gt->ip = buf_p;
        sstrncpy(buf_p, (const char*)row[from + 0], INET6_ADDRSTRLEN + 1);
        buf_p += INET6_ADDRSTRLEN + 1;

        gt->helo = buf_p;
        sstrncpy(buf_p, (const char*)row[from + 1], GREY_MAX_MAIL + 1);
        buf_p += GREY_MAX_MAIL + 1;

        gt->from = buf_p;
        sstrncpy(buf_p, (const char*)row[from + 2], GREY_MAX_MAIL + 1);
        buf_p += GREY_MAX_MAIL + 1;

        gt->to = buf_p;
        sstrncpy(buf_p, (const char*)row[from + 3], GREY_MAX_MAIL + 1);
        buf_p += GREY_MAX_MAIL + 1;
    }
}

static void
populate_val(MYSQL_ROW row, struct DB_val* val, int from)
{
    struct Grey_data* gd;

    memset(val, 0, sizeof(*val));
    val->type = DB_VAL_GREY;
    gd = &val->data.gd;
    gd->first = atoi(row[from + 0]);
    gd->pass = atoi(row[from + 1]);
    gd->expire = atoi(row[from + 2]);
    gd->bcount = atoi(row[from + 3]);
    gd->pcount = atoi(row[from + 4]);
}
