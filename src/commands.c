#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <inttypes.h>
#include "redis.h"
#include "zerodb.h"
#include "index.h"
#include "data.h"

//
// different SET implementation
// depending on the running mode
//
static size_t redis_set_handler_userkey(resp_request_t *request) {
    // create some easier accessor
    unsigned char *id = request->argv[1]->buffer;
    uint8_t idlength = request->argv[1]->length;

    unsigned char *value = request->argv[2]->buffer;
    uint32_t valuelength = request->argv[2]->length;

    debug("[+] set command: %u bytes key, %u bytes data\n", idlength, valuelength);
    // printf("[+] set key: %.*s\n", idlength, id);
    // printf("[+] set value: %.*s\n", request->argv[2]->length, (char *) request->argv[2]->buffer);

    // insert the data on the datafile
    // this will returns us the offset where the header is
    size_t offset = data_insert(value, valuelength, id, idlength);

    // checking for writing error
    // if we couldn't write the data, we won't add entry on the index
    // and report to the client an error
    if(offset == 0) {
        redis_hardsend(request->fd, "$-1");
        return 0;
    }

    debug("[+] userkey: ");
    debughex(id, idlength);
    debug("\n");

    debug("[+] data insertion offset: %lu\n", offset);

    // inserting this offset with the id on the index
    if(!index_entry_insert(id, idlength, offset, request->argv[2]->length)) {
        // cannot insert index (disk issue)
        redis_hardsend(request->fd, "$-1");
        return 0;
    }

    // building response
    // here, from original redis protocol, we don't reply with a basic
    // OK or Error when inserting a key, we reply with the key itself
    //
    // this is how the sequential-id can returns the id generated
    redis_bulk_t response = redis_bulk(id, idlength);
    if(!response.buffer) {
        redis_hardsend(request->fd, "$-1");
        return 0;
    }

    send(request->fd, response.buffer, response.length, 0);
    free(response.buffer);

    return offset;
}

static size_t redis_set_handler_sequential(resp_request_t *request) {
    // create some easier accessor
    uint32_t id = index_next_id();
    uint8_t idlength = sizeof(uint32_t);

    unsigned char *value = request->argv[2]->buffer;
    uint32_t valuelength = request->argv[2]->length;

    debug("[+] set command: %u bytes key, %u bytes data\n", idlength, valuelength);

    // insert the data on the datafile
    // this will returns us the offset where the header is
    // size_t offset = data_insert(value, valuelength, id, idlength);
    size_t offset = data_insert(value, valuelength, &id, idlength);

    // checking for writing error
    // if we couldn't write the data, we won't add entry on the index
    // and report to the client an error
    if(offset == 0) {
        redis_hardsend(request->fd, "$-1");
        return 0;
    }

    debug("[+] generated sequential-key: ");
    debughex(&id, idlength);
    debug("\n");

    debug("[+] data insertion offset: %lu\n", offset);

    // inserting this offset with the id on the index
    // if(!index_entry_insert(id, idlength, offset, request->argv[2]->length)) {
    if(!index_entry_insert(&id, idlength, offset, request->argv[2]->length)) {
        // cannot insert index (disk issue)
        redis_hardsend(request->fd, "$-1");
        return 0;
    }

    // building response
    // here, from original redis protocol, we don't reply with a basic
    // OK or Error when inserting a key, we reply with the key itself
    //
    // this is how the sequential-id can returns the id generated
    // redis_bulk_t response = redis_bulk(id, idlength);
    redis_bulk_t response = redis_bulk(&id, idlength);
    if(!response.buffer) {
        redis_hardsend(request->fd, "$-1");
        return 0;
    }

    send(request->fd, response.buffer, response.length, 0);
    free(response.buffer);

    return offset;
}

static size_t redis_set_handler_directkey(resp_request_t *request) {
    // create some easier accessor
    index_dkey_t id = {
        .dataid = data_dataid(), // current data fileid
        .offset = 0              // will be filled later by data_insert
    };
    uint8_t idlength = sizeof(index_dkey_t);

    unsigned char *value = request->argv[2]->buffer;
    uint32_t valuelength = request->argv[2]->length;

    debug("[+] set command: %u bytes key, %u bytes data\n", idlength, valuelength);

    // insert the data on the datafile
    // this will returns us the offset where the header is
    // size_t offset = data_insert(value, valuelength, id, idlength);
    size_t offset = data_insert(value, valuelength, &id, idlength);
    id.offset = offset;

    // checking for writing error
    // if we couldn't write the data, we won't add entry on the index
    // and report to the client an error
    if(offset == 0) {
        redis_hardsend(request->fd, "$-1");
        return 0;
    }

    debug("[+] generated direct-key: ");
    debughex(&id, idlength);
    debug("\n");

    debug("[+] data insertion offset: %lu\n", offset);

    // usually, here is where we add the key in the index
    // we don't use the index in this case since the position
    // is the key itself

    // building response
    // here, from original redis protocol, we don't reply with a basic
    // OK or Error when inserting a key, we reply with the key itself
    //
    // this is how the sequential-id can returns the id generated
    // redis_bulk_t response = redis_bulk(id, idlength);
    redis_bulk_t response = redis_bulk(&id, idlength);
    if(!response.buffer) {
        redis_hardsend(request->fd, "$-1");
        return 0;
    }

    send(request->fd, response.buffer, response.length, 0);
    free(response.buffer);

    return offset;

    // key = datafile + offset
    // padding to fit num of files/offset in 6 bytes
}

static size_t (*redis_set_handlers[])(resp_request_t *request) = {
    redis_set_handler_userkey,
    redis_set_handler_sequential,
    redis_set_handler_directkey
};


//
// different GET implementation
// depending on the running mode
//
static index_entry_t *redis_get_handler_memkey(resp_request_t *request) {
    return index_entry_get(request->argv[1]->buffer, request->argv[1]->length);
}

static index_entry_t *redis_get_handler_direct(resp_request_t *request) {
    // invalid requested key
    if(request->argv[1]->length != sizeof(index_dkey_t))
        return NULL;

    // converting binary key to internal struct
    index_dkey_t directkey;

    memcpy(&directkey, request->argv[1]->buffer, sizeof(index_dkey_t));
    memcpy(index_reusable_entry->id, request->argv[1]->buffer, sizeof(index_dkey_t));

    index_reusable_entry->idlength = sizeof(index_dkey_t);
    index_reusable_entry->offset = directkey.offset;
    index_reusable_entry->dataid = directkey.dataid;

    // when using a zero-length payload
    // the length will be used from the data header
    // and not from tne index
    index_reusable_entry->length = 0;

    return index_reusable_entry;
}

static index_entry_t * (*redis_get_handlers[])(resp_request_t *request) = {
    redis_get_handler_memkey,
    redis_get_handler_memkey,
    redis_get_handler_direct
};


//
// main worker when a redis command was successfuly parsed
//
int redis_dispatcher(resp_request_t *request) {
    if(request->argv[0]->type != STRING) {
        debug("[+] not a string command, ignoring\n");
        return 0;
    }

    // PING
    if(!strncmp(request->argv[0]->buffer, "PING", request->argv[0]->length)) {
        verbose("[+] redis: PING\n");
        redis_hardsend(request->fd, "+PONG");
        return 0;
    }

    // SET
    // FIXME: rewrite handlers to improve commands
    if(!strncmp(request->argv[0]->buffer, "SET", request->argv[0]->length) || !strncmp(request->argv[0]->buffer, "SETX", request->argv[0]->length)) {
        if(request->argc != 3 || request->argv[2]->length == 0) {
            redis_hardsend(request->fd, "-Invalid argument");
            return 1;
        }

        if(request->argv[1]->length > MAX_KEY_LENGTH) {
            redis_hardsend(request->fd, "-Key too large");
            return 1;
        }

        size_t offset = redis_set_handlers[rootsettings.mode](request);
        if(offset == 0)
            return 0;

        // checking if we need to jump to the next files
        // we do this check here and not from data (event if this is like a
        // datafile event) to keep data and index code completly distinct
        if(offset + request->argv[2]->length > 256 * 1024 * 1024) { // 256 MB
            size_t newid = index_jump_next();
            data_jump_next(newid);
        }

        return 0;
    }

    // GET
    if(!strncmp(request->argv[0]->buffer, "GET", request->argv[0]->length)) {
        if(request->argv[1]->length > MAX_KEY_LENGTH) {
            printf("[-] invalid key size\n");
            redis_hardsend(request->fd, "-Invalid key");
            return 1;
        }

        debug("[+] lookup key: ");
        debughex(request->argv[1]->buffer, request->argv[1]->length);
        debug("\n");

        index_entry_t *entry = redis_get_handlers[rootsettings.mode](request);

        // key not found at all
        if(!entry) {
            verbose("[-] key not found\n");
            redis_hardsend(request->fd, "$-1");
            return 1;
        }

        // key found but deleted
        if(entry->flags & INDEX_ENTRY_DELETED) {
            verbose("[-] key deleted\n");
            redis_hardsend(request->fd, "$-1");
            return 1;
        }

        // key found and valid, let's checking the contents
        debug("[+] entry found, flags: %x, data length: %" PRIu64 "\n", entry->flags, entry->length);
        debug("[+] data file: %d, data offset: %" PRIu64 "\n", entry->dataid, entry->offset);

        data_payload_t payload = data_get(entry->offset, entry->length, entry->dataid, entry->idlength);

        if(!payload.buffer) {
            printf("[-] cannot read payload\n");
            redis_hardsend(request->fd, "-Internal Error");
            free(payload.buffer);
            return 0;
        }

        redis_bulk_t response = redis_bulk(payload.buffer, payload.length);
        if(!response.buffer) {
            redis_hardsend(request->fd, "$-1");
            return 0;
        }

        send(request->fd, response.buffer, response.length, 0);

        free(response.buffer);
        free(payload.buffer);

        return 0;
    }

    if(!strncmp(request->argv[0]->buffer, "DEL", request->argv[0]->length)) {
        if(request->argv[1]->length > MAX_KEY_LENGTH) {
            printf("[-] invalid key size\n");
            redis_hardsend(request->fd, "-Invalid key");
            return 1;
        }

        if(!index_entry_delete(request->argv[1]->buffer, request->argv[1]->length)) {
            redis_hardsend(request->fd, "-Cannot delete key");
            return 0;
        }

        redis_hardsend(request->fd, "+OK");
        return 0;
    }

    #ifndef RELEASE
    // STOP will be only compiled in debug mode
    // this will force to exit listen loop in order to call
    // all destructors, this is useful to ensure every memory allocation
    // are well tracked and well cleaned
    //
    // in production, a user should not be able to stop the daemon
    if(!strncmp(request->argv[0]->buffer, "STOP", request->argv[0]->length)) {
        redis_hardsend(request->fd, "+Stopping");
        return 2;
    }
    #endif

    // unknown
    printf("[-] unsupported redis command\n");
    redis_hardsend(request->fd, "-Command not supported");

    return 1;
}


