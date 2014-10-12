/**
 * @file   test_blacklist.c
 * @brief  Unit tests for the blacklist interface.
 * @author Mikey Austin
 * @date   2014
 */

#include "test.h"
#include "../blacklist.h"
#include "../list.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <arpa/inet.h>

static u_int32_t stoi(const char *address);

int
main()
{
    Blacklist_T bl;
    u_int32_t a1, b1, a2, b2, a3, b3;
    char *s, *s2;
    int i = 0;
    List_T cidrs;
    struct List_entry_T *entry;
    struct IP_cidr *c;
    struct IP_addr a;

    TEST_START(22);

    bl = Blacklist_create("Test List", "You have been blacklisted");
    TEST_OK((bl != NULL), "Blacklist created successfully");
    TEST_OK((strcmp(bl->name, "Test List") == 0), "Name set successfully");
    TEST_OK((strcmp(bl->message, "You have been blacklisted") == 0),
            "Message set successfully");
    TEST_OK((bl->size == BLACKLIST_INIT_SIZE),
            "Correct entry list size ");
    TEST_OK((bl->count == 0), "Correctly initialized list of entries");

    a1 = ntohl(stoi("192.168.1.0"));
    b1 = ntohl(stoi("192.168.1.100"));

    Blacklist_add_range(bl, a1, b1, BL_TYPE_BLACK);
    TEST_OK((bl->count == 2), "Added 2 elements");

    TEST_OK((bl->entries[0].address.v4.s_addr == a1), "First address is correct");
    TEST_OK((bl->entries[0].black == 1), "First address is black");
    TEST_OK((bl->entries[0].white == 0), "First address is not white");

    TEST_OK((bl->entries[1].address.v4.s_addr == b1),
            "Second address is correct");
    TEST_OK((bl->entries[1].black == -1), "Second address is black");
    TEST_OK((bl->entries[1].white == 0), "Second address is not white");

    Blacklist_destroy(&bl);

    /* Test collapsing a blacklist with 3 overlapping regions. */
    bl = Blacklist_create("Test List", "You have been blacklisted");
    a1 = ntohl(stoi("10.0.0.0"));  /* black */
    b1 = ntohl(stoi("10.0.0.20")) + 1;

    a2 = ntohl(stoi("10.0.0.10")); /* black */
    b2 = ntohl(stoi("10.0.0.50")) + 1;

    a3 = ntohl(stoi("10.0.0.40")); /* white */
    b3 = ntohl(stoi("10.0.0.60")) + 1;

    Blacklist_add_range(bl, a1, b1, BL_TYPE_BLACK);
    Blacklist_add_range(bl, a2, b2, BL_TYPE_BLACK);
    Blacklist_add_range(bl, a3, b3, BL_TYPE_WHITE);

    cidrs = Blacklist_collapse(bl);
    LIST_FOREACH(cidrs, entry) {
        c = List_entry_value(entry);
        switch(i) {
        case 0:
            s = "10.0.0.0/27";
            break;

        case 1:
            s = "10.0.0.32/29";
            break;

        default:
            s = "n/a";
        }

        s2 = IP_cidr_to_str(c);
        TEST_OK((strcmp(s, s2) == 0), "Range to CIDR ok");
        free(s2);
        i++;
    }

    List_destroy(&cidrs);
    Blacklist_destroy(&bl);

    /* Test the adding of single addresses & matching of addresses. */
    bl = Blacklist_create("Test List", "You have been blacklisted");
    TEST_OK((Blacklist_add(bl, "192.168.12.1/24") == 0), "IPv4 added OK");
    TEST_OK((Blacklist_add(bl, "10.20.1.3/16") == 0), "IPv4 added OK");
    TEST_OK((bl->count == 2), "Entries added OK");

    a.addr32[0] = ntohl(0xC0A80C23); /* 192.168.12.35 */
    TEST_OK((Blacklist_match(bl, &a, AF_INET) == 1), "IPv4 match as expected");

    a.addr32[0] = ntohl(0xC0A80E23); /* 192.168.14.35 */
    TEST_OK((Blacklist_match(bl, &a, AF_INET) == 0), "IPv4 mismatch as expected");

    a.addr32[0] = ntohl(0x0A146917); /* 10.20.105.23 */
    TEST_OK((Blacklist_match(bl, &a, AF_INET) == 1), "IPv4 match as expected");

    a.addr32[0] = ntohl(0x0A00002D); /* 10.0.0.45 */
    TEST_OK((Blacklist_match(bl, &a, AF_INET) == 0), "IPv4 mismatch as expected");

    Blacklist_destroy(&bl);
    TEST_OK(bl == NULL, "blacklist memory cleaned up and set to NULL");

    TEST_COMPLETE;
}

static u_int32_t
stoi(const char *address)
{
    u_int32_t addr;
    inet_pton(AF_INET, address, &addr);

    return addr;
}
