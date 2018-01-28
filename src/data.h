#ifndef __RKV_DATA_H
    #define __RKV_DATA_H

    // root point of the memory handler
    // used by the data manager
    typedef struct data_t {
        char *datadir;    // root path of the data files
        char *datafile;   // pointer to the current datafile used
        uint16_t dataid;  // id of the datafile currently in use
        int datafd;       // file descriptor of the current datafile used

    } data_t;

    // data_header_t contains header of each entry on the datafile
    // this header doesn't contains the payload, we assume the payload
    // follows the header
    typedef struct data_header_t {
        uint8_t idlength;     // length of the id
        uint32_t datalength;  // length of the payload
        char id[];            // accessor to the id, dynamically

    } __attribute__((packed)) data_header_t;

    void data_init(uint16_t dataid, char *datapath);
    void data_destroy();
    size_t data_jump_next();
    void data_emergency();

    unsigned char *data_get(size_t offset, size_t length, uint16_t dataid, uint8_t idlength);
    size_t data_insert(unsigned char *data, uint32_t datalength, unsigned char *id, uint8_t idlength);
#endif
