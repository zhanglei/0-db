#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <inttypes.h>
#include "zerodb.h"
#include "index.h"
#include "index_scan.h"
#include "data.h"
#include "namespace.h"
#include "redis.h"
#include "commands.h"
#include "commands_get.h"
#include "commands_scan.h"

uint64_t ustime() {
    struct timeval tv;
    uint64_t ust;

    gettimeofday(&tv, NULL);

    ust = ((uint64_t) tv.tv_sec) * 1000000;
    ust += tv.tv_usec;

    return ust;
}

//
// scan list management
//

// append one entry into the scanlist result
// if the list is not large enough to contains the entry
// growing it up a little bit
static scan_list_t *scanlist_append(scan_list_t *scanlist, index_scan_t *scan) {
    if(scanlist->length + 1 > scanlist->allocated) {
        scanlist->allocated += 32;

        if(!(scanlist->items = realloc(scanlist->items, scanlist->allocated * sizeof(index_item_t *))))
            return NULL;

        if(!(scanlist->offsets = realloc(scanlist->offsets, scanlist->allocated * sizeof(uint32_t))))
            return NULL;
    }

    scanlist->items[scanlist->length] = scan->header;
    scanlist->offsets[scanlist->length] = scan->target;
    scanlist->length += 1;

    return scanlist;
}

static void scanlist_init(scan_list_t *scanlist) {
    memset(scanlist, 0x00, sizeof(scan_list_t));
}

static void scanlist_free(scan_list_t *scanlist) {
    for(size_t i = 0; i < scanlist->length; i++)
        free(scanlist->items[i]);

    free(scanlist->items);
    free(scanlist->offsets);
}

static void scaninfo_from_scan(scan_info_t *info, index_scan_t *scan) {
    info->dataid = scan->header->dataid;
    info->idxoffset = scan->target;
}

static void scaninfo_from_entry(scan_info_t *info, index_entry_t *entry) {
    info->dataid = entry->dataid;
    info->idxoffset = entry->idxoffset;
}

//
// redis serialization of the scan list
//
static int command_scan_send_scanlist(scan_list_t *scanlist, redis_client_t *client) {
    char *response;
    size_t offset = 0;
    index_item_t *entry;
    index_bkey_t bkey;
    uint32_t idxoffset;

    // if the list is empty, we have nothing
    // to send, obviously
    if(scanlist->length == 0) {
        redis_hardsend(client, "-No more data");
        return 0;
    }

    // array response, with 2 arguments:
    //  - first one is the next SCAN key value
    //    (in our case, this is always the same value as the returned id)
    //  - the second one is another array, of each keys found, each entry containins
    //    information about this key like timestamp and size
    if(!(response = malloc(((MAX_KEY_LENGTH * 2) + 128) * scanlist->length)))
        return 1;

    // converting the last object key into a binary serialized key
    entry = scanlist->items[scanlist->length - 1];
    idxoffset = scanlist->offsets[scanlist->length - 1];
    bkey = index_item_serialize(entry, idxoffset);

    // get last entry for the next key value
    offset = sprintf(response, "*2\r\n$%ld\r\n", sizeof(index_bkey_t));

    // copy the key
    memcpy(response + offset, &bkey, sizeof(index_bkey_t));
    offset += sizeof(index_bkey_t);

    // iterating over the full list and building the list response
    offset += sprintf(response + offset, "\r\n*%lu\r\n", scanlist->length);

    for(size_t i = 0; i < scanlist->length; i++) {
        entry = scanlist->items[i];

        // end of the key
        // adding the array of response with the single response
        offset += sprintf(response + offset, "*3\r\n");

        // adding the key
        offset += sprintf(response + offset, "$%u\r\n", entry->idlength);
        memcpy(response + offset, entry->id, entry->idlength);
        offset += entry->idlength;

        // adding the length of the payload
        offset += sprintf(response + offset, "\r\n:%d\r\n", entry->length);

        // adding the timestamp to the payload
        offset += sprintf(response + offset, ":%d\r\n", entry->timestamp);
    }

    redis_reply(client, response, offset);
    free(response);

    return 0;
}

static scan_info_t *scan_initial_info(scan_info_t *info, scan_list_t *scanlist, index_scan_t *scan) {
    // could not get initial scan entry
    if(scan->status != INDEX_SCAN_SUCCESS)
        return NULL;

    scanlist_append(scanlist, scan);
    scaninfo_from_scan(info, scan);

    return info;
}

static scan_info_t *scan_initial_get(scan_info_t *info, redis_client_t *client) {
    index_entry_t *entry = NULL;
    index_bkey_t bkey;

    if(client->request->argv[1]->length != sizeof(index_bkey_t)) {
        debug("[-] command: scan: requested key invalid (size mismatch)\n");
        redis_hardsend(client, "-Invalid key format");
        return NULL;
    }

    memcpy(&bkey, client->request->argv[1]->buffer, sizeof(index_bkey_t));

    if(!(entry = index_entry_deserialize(client->ns->index, &bkey))) {
        debug("[-] command: scan: could not fetch/validate key requested\n");
        redis_hardsend(client, "-Invalid key format");
        return NULL;
    }

    #if 0
    // grabbing original entry
    if(!(entry = redis_get_handlers[rootsettings.mode](client))) {
        debug("[-] command: scan: key not found\n");
        redis_hardsend(client, "-Invalid index");
        return NULL;
    }

    if(index_entry_is_deleted(entry)) {
        verbose("[-] command: scan: key deleted\n");
        redis_hardsend(client, "-Invalid index");
        return NULL;
    }
    #endif

    scaninfo_from_entry(info, entry);
    free(entry);

    return info;
}

static int scan_failure(index_scan_t *scan, redis_client_t *client) {
    if(scan->status == INDEX_SCAN_NO_MORE_DATA)
        redis_hardsend(client, "-No more data");

    if(scan->status == INDEX_SCAN_UNEXPECTED)
        redis_hardsend(client, "-Internal Error");

    return 1;
}

//
// SCAN
//
int command_scan(redis_client_t *client) {
    index_scan_t scan;
    scan_list_t scanlist;
    scan_info_t info;

    // initialize empty scanlist
    scanlist_init(&scanlist);

    // scan requested without initial key
    if(client->request->argc == 1) {
        scan = index_first_header(client->ns->index);

        if(!scan_initial_info(&info, &scanlist, &scan))
            return scan_failure(&scan, client);

    } else {
        // scan requested with an initial key
        if(!scan_initial_get(&info, client))
            return 1;
    }

    // we have everything needed to start walking over
    // the keys and building our scan response
    uint64_t basetime = ustime();

    while(ustime() - basetime < SCAN_TIMESLICE_US) {
        debug("[+] scan: elapsed time: %lu us\n", ustime() - basetime);

        // reading entry and appending it
        scan = index_next_header(client->ns->index, info.dataid, info.idxoffset);

        // this scan failed, let's guess it's the end
        if(scan.status != INDEX_SCAN_SUCCESS)
            break;

        scanlist_append(&scanlist, &scan);

        // preparing next call
        scaninfo_from_scan(&info, &scan);
    }

    debug("[+] scan: retreived %lu entries in %lu us\n", scanlist.length, ustime() - basetime);

    if(command_scan_send_scanlist(&scanlist, client))
        redis_hardsend(client, "-Internal Error");

    scanlist_free(&scanlist);
    return 0;
}

//
// RSCAN
//
int command_rscan(redis_client_t *client) {
    index_scan_t scan;
    scan_list_t scanlist;
    scan_info_t info;

    // initialize empty scanlist
    scanlist_init(&scanlist);

    // scan requested without initial key
    if(client->request->argc == 1) {
        scan = index_last_header(client->ns->index);

        if(!scan_initial_info(&info, &scanlist, &scan))
            return scan_failure(&scan, client);

    } else {
        // scan requested with an initial key
        if(!scan_initial_get(&info, client))
            return 1;
    }

    // we have everything needed to start walking over
    // the keys and building our scan response
    uint64_t basetime = ustime();

    while(ustime() - basetime < SCAN_TIMESLICE_US) {
        debug("[+] scan: elapsed time: %lu us\n", ustime() - basetime);

        // reading entry and appending it
        scan = index_previous_header(client->ns->index, info.dataid, info.idxoffset);

        // this scan failed, let's guess it's the end
        if(scan.status != INDEX_SCAN_SUCCESS)
            break;

        scanlist_append(&scanlist, &scan);

        // preparing next call
        scaninfo_from_scan(&info, &scan);
    }

    debug("[+] scan: retreived %lu entries in %lu us\n", scanlist.length, ustime() - basetime);

    if(command_scan_send_scanlist(&scanlist, client))
        redis_hardsend(client, "-Internal Error");

    scanlist_free(&scanlist);
    return 0;
}

