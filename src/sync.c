/*
 * Copyright (c) 2014 Mikey Austin <mikey@jackiemclean.net>
 * Copyright (c) 2006, 2007 Reyk Floeter <reyk@openbsd.org>
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
 * @file   sync.c
 * @brief  Implements the multicast/UDP sync engine interface.
 * @author Mikey Austin
 * @date   2014
 */

#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <netdb.h>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "config_value.h"
#include "constants.h"
#include "failures.h"
#include "grey.h"
#include "list.h"
#include "sync.h"
#include "utils.h"

#define KEY_BUF_SIZE 512

struct Sync_host {
    char* name;
    struct sockaddr_in addr;
};

static void send_sync_message(Sync_engine_T, struct iovec*, int);
static void send_address(Sync_engine_T, char*, time_t, time_t, u_int16_t);
static void destroy_sync_host(void*);

extern Sync_engine_T
Sync_init(Config_T config)
{
    Sync_engine_T engine;
    char *key_path, buf[KEY_BUF_SIZE];
    unsigned char* p;
    Config_value_T val;
    List_T sync_hosts;
    struct List_entry* entry;
    int fd, nread = 0, i;
    SHA_CTX ctx;
    unsigned char digest[SYNC_HMAC_LEN];
    static const char hex[] = "0123456789abcdef";

    if (!Config_get_int(config, "enable", "sync", 0))
        return NULL;

    if ((engine = calloc(1, sizeof(*engine))) == NULL)
        err(1, "calloc");

    engine->sync_fd = -1;
    engine->config = config;
    engine->grey_out = NULL;
    engine->iface = NULL;
    engine->sync_hosts = List_create(destroy_sync_host);
    engine->port = Config_get_int(config, "port", "sync", GREYD_SYNC_PORT);

    if ((sync_hosts = Config_get_list(config, "hosts", "sync")) != NULL) {
        LIST_EACH(sync_hosts, entry)
        {
            val = List_entry_value(entry);
            if (cv_str(val) && Sync_add_host(engine, cv_str(val)) != 0) {
                /*
                 * This was not a host address, so treat it as an
                 * interface name.
                 */
                engine->iface = cv_str(val);
            }
        }
    }

    /* Use empty key by default. */
    *engine->sync_key = '\0';
    if (Config_get_int(config, "verify", "sync", SYNC_VERIFY_MSG)) {
        key_path = Config_get_str(config, "key", "sync", SYNC_KEY);
        if ((fd = open(key_path, O_RDONLY)) < 0) {
            if (errno != ENOENT) {
                i_warning("failed to open sync key: %s",
                    strerror(errno));
                return NULL;
            }
        } else {
            memset(&ctx, 0, sizeof(ctx));
            SHA1_Init(&ctx);
            while ((nread = read(fd, buf, KEY_BUF_SIZE)) != 0) {
                if (nread == -1) {
                    i_warning("failed to read in sync key: %s",
                        strerror(errno));
                    return NULL;
                }
                SHA1_Update(&ctx, buf, nread);
            }
            close(fd);
            SHA1_Final(digest, &ctx);

            /*
             * The following manipulation of the digest was pulled from sha1hl.c, to make
             * the above OpenSSL generated HMAC be compatible with the OpenBSD SHA1File
             * helper function. License from original file kept intact below (possibly overkill).
             *
             * ----------------------------------------------------------------------------
             * "THE BEER-WARE LICENSE" (Revision 42):
             * <phk@login.dkuug.dk> wrote this file.  As long as you retain this notice you
             * can do whatever you want with this stuff. If we meet some day, and you think
             * this stuff is worth it, you can buy me a beer in return.   Poul-Henning Kamp
             * ----------------------------------------------------------------------------
             */
            p = engine->sync_key;
            for (i = 0; i < 20; i++) {
                p[i + i] = hex[((u_int32_t)digest[i]) >> 4];
                p[i + i + 1] = hex[digest[i] & 0x0f];
            }
            p[i + i] = '\0';
        }
    }

    return engine;
}

extern int
Sync_start(Sync_engine_T engine)
{
    char* bind_addr;
    int one = 1;
    u_int8_t ttl;
    struct ifreq ifr;
    struct ip_mreq mreq;
    struct sockaddr_in* addr;
    char if_name[IFNAMSIZ], *ttl_str;
    char *end, *mcast_addr;
    struct in_addr bind_in_addr;

    bind_addr = Config_get_str(engine->config, "bind_address", "sync", NULL);

    if (engine->iface != NULL)
        engine->send_mcast++;

    memset(&bind_in_addr, 0, sizeof(bind_in_addr));
    if (bind_addr != NULL) {
        if (inet_pton(AF_INET, bind_addr, &bind_in_addr) != 1) {
            /*
             * As bind_addr is not an address, treat it as an interface.
             */
            bind_in_addr.s_addr = htonl(INADDR_ANY);
            if (engine->iface == NULL) {
                engine->iface = bind_addr;
            } else if (strcmp(bind_addr, engine->iface) != 0) {
                i_warning("multicast interface %s does not match %s",
                    bind_addr, engine->iface);
                return -1;
            }
        }
    }

    engine->sync_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (engine->sync_fd == -1) {
        i_warning("socket: %s", strerror(errno));
        return -1;
    }

    if (setsockopt(engine->sync_fd, SOL_SOCKET, SO_REUSEADDR, &one,
            sizeof(one))
        == -1) {
        i_warning("setsockopt: %s", strerror(errno));
        goto fail;
    }

    memset(&engine->sync_out, 0, sizeof(engine->sync_out));
    engine->sync_out.sin_family = AF_INET;
    engine->sync_out.sin_addr.s_addr = bind_in_addr.s_addr;
    if (bind_addr == NULL && engine->iface == NULL)
        engine->sync_out.sin_port = 0;
    else
        engine->sync_out.sin_port = htons(engine->port);

    if (bind(engine->sync_fd, (struct sockaddr*)&engine->sync_out,
            sizeof(engine->sync_out))
        == -1) {
        i_warning("bind: %s", strerror(errno));
        goto fail;
    }

    if (engine->iface == NULL) {
        /* Don't use multicast messages. */
        return engine->sync_fd;
    }

    /* Extract any TTL value from the interface name. */
    sstrncpy(if_name, engine->iface, sizeof(if_name));
    ttl = Config_get_int(engine->config, "ttl", "sync", SYNC_MCASTTTL);
    if ((ttl_str = strchr(if_name, ':')) != NULL) {
        *ttl_str++ = '\0';
        errno = 0;
        ttl = strtol(ttl_str, &end, 10);
        if (ttl == 0 || ttl_str[0] == '\0' || *end != '\0'
            || (errno == ERANGE)) {
            i_warning("invalid multicast ttl %s", ttl_str);
            goto fail;
        }
    }

    /* Extract the IP address from the interface. */
    memset(&ifr, 0, sizeof(ifr));
    sstrncpy(ifr.ifr_name, if_name, sizeof(ifr.ifr_name));
    if (ioctl(engine->sync_fd, SIOCGIFADDR, &ifr) == -1) {
        i_warning("ioctl SIOCGIFADDR %s: %s", if_name,
            strerror(errno));
        goto fail;
    }

    memset(&engine->sync_in, 0, sizeof(engine->sync_in));
    addr = (struct sockaddr_in*)&ifr.ifr_addr;
    engine->sync_in.sin_family = AF_INET;
    engine->sync_in.sin_addr.s_addr = addr->sin_addr.s_addr;
    engine->sync_in.sin_port = htons(engine->port);

    mcast_addr = Config_get_str(engine->config, "mcast_address", "sync",
        SYNC_MCASTADDR);
    memset(&mreq, 0, sizeof(mreq));
    engine->sync_out.sin_addr.s_addr = inet_addr(mcast_addr);
    mreq.imr_multiaddr.s_addr = inet_addr(mcast_addr);
    mreq.imr_interface.s_addr = engine->sync_in.sin_addr.s_addr;

    if (setsockopt(engine->sync_fd, IPPROTO_IP,
            IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq))
        == -1) {
        i_warning("failed to add multicast membership to %s: %s",
            mcast_addr, strerror(errno));
        goto fail;
    }

    if (setsockopt(engine->sync_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl,
            sizeof(ttl))
        < 0) {
        i_warning("failed to set multicast ttl to %u: %s",
            ttl, strerror(errno));
        setsockopt(engine->sync_fd, IPPROTO_IP,
            IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        goto fail;
    }

    i_debug("using multicast spam sync %smode "
            "(ttl %u, group %s, port %d)",
        engine->send_mcast ? "" : "receive ",
        ttl, inet_ntoa(engine->sync_out.sin_addr), engine->port);

    return engine->sync_fd;

fail:
    close(engine->sync_fd);
    return -1;
}

extern void
Sync_stop(Sync_engine_T* engine)
{
    if (engine && *engine) {
        if ((*engine)->sync_hosts)
            List_destroy(&(*engine)->sync_hosts);
        free(*engine);
        *engine = NULL;
    }
}

extern int
Sync_add_host(Sync_engine_T engine, const char* name)
{
    struct addrinfo hints, *res, *next;
    struct Sync_host* host;
    struct sockaddr_in* addr = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(name, NULL, &hints, &next) != 0)
        return EINVAL;

    for (res = next; res != NULL; res = res->ai_next) {
        if (addr == NULL && res->ai_family == AF_INET) {
            addr = (struct sockaddr_in*)res->ai_addr;
            break;
        }
    }

    if (addr == NULL) {
        freeaddrinfo(next);
        return EINVAL;
    }

    if ((host = calloc(1, sizeof(*host))) == NULL) {
        freeaddrinfo(next);
        return ENOMEM;
    }

    if ((host->name = strdup(name)) == NULL) {
        free(host);
        freeaddrinfo(next);
        return ENOMEM;
    }

    host->addr.sin_family = AF_INET;
    host->addr.sin_port = htons(engine->port);
    host->addr.sin_addr.s_addr = addr->sin_addr.s_addr;
    freeaddrinfo(next);

    List_insert_after(engine->sync_hosts, host);
    i_debug("added spam sync host %s (address %s, port %d)",
        host->name, inet_ntoa(host->addr.sin_addr),
        engine->port);

    return 0;
}

extern void
Sync_recv(Sync_engine_T engine, FILE* grey_out)
{
    struct Sync_hdr* hdr;
    struct sockaddr_in addr;
    struct Sync_tlv_hdr* tlv;
    struct Sync_tlv_grey* sg;
    struct Sync_tlv_addr* sd;
    u_int8_t buf[SYNC_MAXSIZE];
    u_int8_t hmac[2][SYNC_HMAC_LEN];
    struct in_addr ip;
    char *from, *to, *helo;
    char src_ip[INET_ADDRSTRLEN + 1];
    u_int8_t* p;
    socklen_t addr_len;
    ssize_t len;
    short delete = 0;
    u_int hmac_len;
    u_int32_t expire;

    memset(&addr, 0, sizeof(addr));
    memset(buf, 0, sizeof(buf));

    addr_len = sizeof(addr);
    if ((len = recvfrom(engine->sync_fd, buf, sizeof(buf), 0,
             (struct sockaddr*)&addr, &addr_len))
        < 1) {
        return;
    }

    if (addr.sin_addr.s_addr != htonl(INADDR_ANY) && memcmp(&engine->sync_in.sin_addr, &addr.sin_addr, sizeof(addr.sin_addr)) == 0) {
        return;
    }

    /* Ignore invalid or truncated packets. */
    hdr = (struct Sync_hdr*)buf;
    if (len < sizeof(struct Sync_hdr) || hdr->sh_version != SYNC_VERSION || hdr->sh_af != AF_INET || len < ntohs(hdr->sh_length)) {
        goto trunc;
    }
    len = ntohs(hdr->sh_length);

    /* Compute and validate HMAC. */
    memcpy(hmac[0], hdr->sh_hmac, SYNC_HMAC_LEN);
    memset(hdr->sh_hmac, 0, SYNC_HMAC_LEN);
    HMAC(EVP_sha1(), engine->sync_key, sizeof(engine->sync_key),
        buf, len, hmac[1], &hmac_len);
    if (memcmp(hmac[0], hmac[1], SYNC_HMAC_LEN) != 0)
        goto trunc;

    sstrncpy(src_ip, inet_ntoa(addr.sin_addr), sizeof(src_ip));
    i_debug("%s (sync): received packet of %d bytes",
        src_ip, (int)len);

    p = (u_int8_t*)(hdr + 1);
    while (len) {
        tlv = (struct Sync_tlv_hdr*)p;

        if (len < sizeof(struct Sync_tlv_hdr) || len < ntohs(tlv->st_length)) {
            goto trunc;
        }

        switch (ntohs(tlv->st_type)) {
        case SYNC_GREY:
            sg = (struct Sync_tlv_grey*)tlv;
            if ((sizeof(*sg) + ntohs(sg->sg_from_length) + ntohs(sg->sg_to_length) + ntohs(sg->sg_helo_length)) > ntohs(tlv->st_length)) {
                goto trunc;
            }

            ip.s_addr = sg->sg_ip;
            from = (char*)(sg + 1);
            to = from + ntohs(sg->sg_from_length);
            helo = to + ntohs(sg->sg_to_length);

            i_debug("%s (sync): received grey entry "
                    "from %s to %s, helo %s ip %s",
                src_ip, from, to, helo, inet_ntoa(ip));

            if (Config_get_int(engine->config, "enable", "grey",
                    GREYLISTING_ENABLED)) {
                /* Send this info to the greylister. */
                fprintf(grey_out,
                    "type = %d\n"
                    "sync = 0\n"
                    "ip = \"%s\"\n"
                    "helo = \"%s\"\n"
                    "from = \"%s\"\n"
                    "to = \"%s\"\n"
                    "%%\n",
                    GREY_MSG_GREY, inet_ntoa(ip),
                    helo, from, to);
                fflush(grey_out);
            }
            break;

        case SYNC_DEL_WHITE:
            delete = 1;
            /* Fallthrough. */

        case SYNC_WHITE:
            sd = (struct Sync_tlv_addr*)tlv;
            if (sizeof(*sd) != ntohs(tlv->st_length))
                goto trunc;

            ip.s_addr = sd->sd_ip;
            expire = ntohl(sd->sd_expire);
            i_debug("%s (sync): received white entry ip %s (%s) ",
                src_ip, inet_ntoa(ip),
                delete ? "deletion" : "addition");

            if (Config_get_int(engine->config, "enable", "grey",
                    GREYLISTING_ENABLED)) {
                /* Send this info to the greylister. */
                fprintf(grey_out,
                    "type = %d\n"
                    "sync = 0\n"
                    "ip = \"%s\"\n"
                    "source = \"%s\"\n"
                    "expires = \"%u\"\n"
                    "delete = %u\n"
                    "%%\n",
                    GREY_MSG_WHITE, inet_ntoa(ip),
                    src_ip, expire, delete);
                fflush(grey_out);
            }
            break;

        case SYNC_DEL_TRAPPED:
            delete = 1;
            /* Fallthrough. */

        case SYNC_TRAPPED:
            sd = (struct Sync_tlv_addr*)tlv;
            if (sizeof(*sd) != ntohs(tlv->st_length))
                goto trunc;

            ip.s_addr = sd->sd_ip;
            expire = ntohl(sd->sd_expire);
            i_debug("%s (sync): received trapped entry ip %s (%s) ",
                src_ip, inet_ntoa(ip),
                delete ? "deletion" : "addition");

            if (Config_get_int(engine->config, "enable", "grey",
                    GREYLISTING_ENABLED)) {
                /* Send this info to the greylister. */
                fprintf(grey_out,
                    "type = %d\n"
                    "sync = 0\n"
                    "ip = \"%s\"\n"
                    "source = \"%s\"\n"
                    "expires = \"%u\"\n"
                    "delete = %u\n"
                    "%%\n",
                    GREY_MSG_TRAP, inet_ntoa(ip),
                    src_ip, expire, delete);
                fflush(grey_out);
            }
            break;

        case SYNC_END:
            goto done;

        default:
            i_warning("unknown type: %d", ntohs(tlv->st_type));
            goto trunc;
        }

        len -= ntohs(tlv->st_length);
        p = ((u_int8_t*)tlv) + ntohs(tlv->st_length);
    }

done:
    return;

trunc:
    i_debug("%s (sync): truncated or invalid packet", src_ip);
}

extern void
Sync_update(Sync_engine_T engine, struct Grey_tuple* gt, time_t now)
{
    struct iovec iov[7];
    struct Sync_hdr hdr;
    struct Sync_tlv_grey sg;
    struct Sync_tlv_hdr end;
    u_int16_t sglen, fromlen, tolen, helolen, padlen;
    char pad[SYNC_ALIGNBYTES];
    int i = 0;
    u_int hmac_len;

    i_debug("sync grey update helo %s ip %s from %s to %s",
        gt->helo, gt->ip, gt->from, gt->to);

    memset(&hdr, 0, sizeof(hdr));
    memset(&sg, 0, sizeof(sg));
    memset(&pad, 0, sizeof(pad));

    fromlen = strlen(gt->from) + 1;
    tolen = strlen(gt->to) + 1;
    helolen = strlen(gt->helo) + 1;

#ifdef OPENSSL_PRE_1_1_COMPAT
    HMAC_CTX _ctx, *ctx = &_ctx;
    HMAC_CTX_init(ctx);
    HMAC_Init(ctx, engine->sync_key, sizeof(engine->sync_key),
        EVP_sha1());
#else
    HMAC_CTX* ctx = HMAC_CTX_new();
    HMAC_Init_ex(ctx, engine->sync_key, sizeof(engine->sync_key),
        EVP_sha1(), NULL);
#endif

    sglen = sizeof(sg) + fromlen + tolen + helolen;
    padlen = SYNC_ALIGN(sglen) - sglen;

    /* Add SPAM sync packet header */
    hdr.sh_version = SYNC_VERSION;
    hdr.sh_af = AF_INET;
    hdr.sh_counter = htonl(engine->sync_counter++);
    hdr.sh_length = htons(sizeof(hdr) + sglen + padlen + sizeof(end));
    iov[i].iov_base = &hdr;
    iov[i].iov_len = sizeof(hdr);
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    /* Add single SPAM sync greylisting entry */
    sg.sg_type = htons(SYNC_GREY);
    sg.sg_length = htons(sglen + padlen);
    sg.sg_timestamp = htonl(now);
    sg.sg_ip = inet_addr(gt->ip);
    sg.sg_from_length = htons(fromlen);
    sg.sg_to_length = htons(tolen);
    sg.sg_helo_length = htons(helolen);
    iov[i].iov_base = &sg;
    iov[i].iov_len = sizeof(sg);
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    iov[i].iov_base = gt->from;
    iov[i].iov_len = fromlen;
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    iov[i].iov_base = gt->to;
    iov[i].iov_len = tolen;
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    iov[i].iov_base = gt->helo;
    iov[i].iov_len = helolen;
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    iov[i].iov_base = pad;
    iov[i].iov_len = padlen;
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    /* Add end marker */
    end.st_type = htons(SYNC_END);
    end.st_length = htons(sizeof(end));
    iov[i].iov_base = &end;
    iov[i].iov_len = sizeof(end);
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    HMAC_Final(ctx, hdr.sh_hmac, &hmac_len);

    /* Send message to the target hosts. */
    send_sync_message(engine, iov, i);
#ifdef OPENSSL_PRE_1_1_COMPAT
    HMAC_CTX_cleanup(ctx);
#else
    HMAC_CTX_free(ctx);
#endif
}

extern void
Sync_white(Sync_engine_T engine, char* ip, time_t now, time_t expire, short delete)
{
    send_address(engine, ip, now, expire, (delete ? SYNC_DEL_WHITE : SYNC_WHITE));
}

extern void
Sync_trapped(Sync_engine_T engine, char* ip, time_t now, time_t expire, short delete)
{
    send_address(engine, ip, now, expire, (delete ? SYNC_DEL_TRAPPED : SYNC_TRAPPED));
}

static void
send_address(Sync_engine_T engine, char* ip, time_t now, time_t expire, u_int16_t type)
{
    struct iovec iov[3];
    struct Sync_hdr hdr;
    struct Sync_tlv_addr sd;
    struct Sync_tlv_hdr end;
    int i = 0;
    u_int hmac_len;
    char* type_name = "";

    switch (type) {
    case SYNC_WHITE:
        type_name = "white";
        break;

    case SYNC_DEL_WHITE:
        type_name = "deletion of white";
        break;

    case SYNC_TRAPPED:
        type_name = "trapped";
        break;

    case SYNC_DEL_TRAPPED:
        type_name = "deletion of trapped";
        break;
    }
    i_debug("sync %s %s", type_name, ip);

    memset(&hdr, 0, sizeof(hdr));
    memset(&sd, 0, sizeof(sd));

#ifdef OPENSSL_PRE_1_1_COMPAT
    HMAC_CTX _ctx, *ctx = &_ctx;
    HMAC_CTX_init(ctx);
    HMAC_Init(ctx, engine->sync_key, sizeof(engine->sync_key),
        EVP_sha1());
#else
    HMAC_CTX* ctx = HMAC_CTX_new();
    HMAC_Init_ex(ctx, engine->sync_key, sizeof(engine->sync_key), EVP_sha1(), NULL);
#endif

    /* Add SPAM sync packet header. */
    hdr.sh_version = SYNC_VERSION;
    hdr.sh_af = AF_INET;
    hdr.sh_counter = htonl(engine->sync_counter++);
    hdr.sh_length = htons(sizeof(hdr) + sizeof(sd) + sizeof(end));
    iov[i].iov_base = &hdr;
    iov[i].iov_len = sizeof(hdr);
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    /* Add single SPAM sync address entry */
    sd.sd_type = htons(type);
    sd.sd_length = htons(sizeof(sd));
    sd.sd_timestamp = htonl(now);
    sd.sd_expire = htonl(expire);
    sd.sd_ip = inet_addr(ip);
    iov[i].iov_base = &sd;
    iov[i].iov_len = sizeof(sd);
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    /* Add end marker */
    end.st_type = htons(SYNC_END);
    end.st_length = htons(sizeof(end));
    iov[i].iov_base = &end;
    iov[i].iov_len = sizeof(end);
    HMAC_Update(ctx, iov[i].iov_base, iov[i].iov_len);
    i++;

    HMAC_Final(ctx, hdr.sh_hmac, &hmac_len);

    /* Send message to the target hosts. */
    send_sync_message(engine, iov, i);
#ifdef OPENSSL_PRE_1_1_COMPAT
    HMAC_CTX_cleanup(ctx);
#else
    HMAC_CTX_free(ctx);
#endif
}

static void
send_sync_message(Sync_engine_T engine, struct iovec* iov, int iovlen)
{
    struct List_entry* entry;
    struct Sync_host* host;
    struct msghdr msg;

    /* setup buffer */
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = iovlen;

    if (engine->send_mcast) {
        i_debug("sending multicast sync message");
        msg.msg_name = &engine->sync_out;
        msg.msg_namelen = sizeof(engine->sync_out);

        if (sendmsg(engine->sync_fd, &msg, 0) == -1)
            i_warning("sendmsg: %s", strerror(errno));
    }

    LIST_EACH(engine->sync_hosts, entry)
    {
        host = List_entry_value(entry);
        i_debug("sending sync message to %s (%s)",
            host->name, inet_ntoa(host->addr.sin_addr));
        msg.msg_name = &host->addr;
        msg.msg_namelen = sizeof(host->addr);

        if (sendmsg(engine->sync_fd, &msg, 0) == -1)
            i_warning("sendmsg: %s", strerror(errno));
    }
}

static void
destroy_sync_host(void* entry)
{
    struct Sync_host* host = entry;

    if (host) {
        if (host->name) {
            free(host->name);
            host->name = NULL;
        }
        free(host);
    }
}
