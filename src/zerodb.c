#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <execinfo.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include "zerodb.h"
#include "redis.h"
#include "index.h"
#include "data.h"

//
// global system settings
//
settings_t rootsettings = {
    .datapath = "./zdb-data",
    .indexpath = "./zdb-index",
    .listen = "0.0.0.0",
    .port = 9900,
    .verbose = 0,
    .dump = 0,
    .sync = 0,
};

static struct option long_options[] = {
    {"data",    required_argument, 0, 'd'},
    {"index",   required_argument, 0, 'i'},
    {"listen",  required_argument, 0, 'l'},
    {"port",    required_argument, 0, 'p'},
    {"verbose", no_argument,       0, 'v'},
    {"sync",    no_argument,       0, 's'},
    {"dump",    no_argument,       0, 'x'},
    {"help",    no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

//
// system directory management
//
int dir_exists(char *path) {
    struct stat sb;
    return (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode));
}

void dir_create(char *path) {
    char tmp[PATH_MAX], *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if(tmp[len - 1] == '/')
        tmp[len - 1] = 0;

    for(p = tmp + 1; *p; p++) {
        if(*p == '/') {
            *p = 0;
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    }

    mkdir(tmp, S_IRWXU);
}

//
// global warning and fatal message
//
void warnp(char *str) {
    fprintf(stderr, "[-] %s: %s\n", str, strerror(errno));
}

void diep(char *str) {
    warnp(str);
    exit(EXIT_FAILURE);
}

static int signal_intercept(int signal, void (*function)(int)) {
    struct sigaction sig;
    int ret;

    sigemptyset(&sig.sa_mask);
    sig.sa_handler = function;
    sig.sa_flags   = 0;

    if((ret = sigaction(signal, &sig, NULL)) == -1)
        diep("sigaction");

    return ret;
}

// signal handler will take care to try to
// save as much as possible, when problem occures
// for exemple, on segmentation fault, we will try to flush
// and closes descriptor anyway to avoid loosing data
static void sighandler(int signal) {
    void *buffer[1024];

    switch(signal) {
        case SIGSEGV:
            fprintf(stderr, "[-] fatal: segmentation fault\n");
            fprintf(stderr, "[-] ----------------------------------\n");

            int calls = backtrace(buffer, sizeof(buffer) / sizeof(void *));
			backtrace_symbols_fd(buffer, calls, 1);

            fprintf(stderr, "[-] ----------------------------------");

            // no break, we will execute SIGINT handler in SIGSEGV
            // which will try to save and flush buffers

        case SIGINT:
            printf("\n[+] flushing index and data\n");
            index_emergency();
            data_emergency();

        break;
    }

    // forwarding original error code
    exit(128 + signal);
}


static int proceed(struct settings_t *settings) {
    verbose("[+] setting up environments\n");
    signal_intercept(SIGSEGV, sighandler);
    signal_intercept(SIGINT, sighandler);

    // creating the index in memory
    // this will returns us the id of the index
    // file currently used, this is needed by the data
    // storage to keep files linked (index-0067 <> data-0067)
    uint16_t indexid = index_init(settings->indexpath, settings->dump, settings->sync);
    data_init(indexid, settings->datapath, settings->sync);

    // main worker point
    redis_listen(settings->listen, settings->port);

    // we should not reach this point in production
    // this case is handled when calling explicitly
    // a STOP to the server to gracefuly quit
    //
    // this is useful when profiling to ensure there
    // is no memory leaks, if everything is cleaned as
    // expected.
    index_destroy();
    data_destroy();

    return 0;
}

void usage() {
    printf("Command line arguments:\n");
    printf("  --data      datafile directory (default ./data)\n");
    printf("  --index     indexfiles directory (default ./index)\n");
    printf("  --listen    listen address (default 0.0.0.0)\n");
    printf("  --port      listen port (default 9900)\n");
    printf("  --verbose   enable verbose (debug) information\n");
    printf("  --dump      only dump index contents (debug)\n");
    printf("  --sync      force all write to be sync'd\n");
    printf("  --help      print this message\n");

    exit(EXIT_FAILURE);
}

//
// main entry: processing arguments
//
int main(int argc, char *argv[]) {
    printf("[*] Zero-DB (0-db), unstable version\n");

    settings_t *settings = &rootsettings;
    int option_index = 0;

    while(1) {
        // int i = getopt_long_only(argc, argv, "d:i:l:p:vxh", long_options, &option_index);
        int i = getopt_long_only(argc, argv, "", long_options, &option_index);

        if(i == -1)
            break;

        switch(i) {
            case 'd':
                settings->datapath = optarg;
                break;

            case 'i':
                settings->indexpath = optarg;
                break;

            case 'l':
                settings->listen = optarg;
                break;

            case 'p':
                settings->port = atoi(optarg);
                break;

            case 'v':
                settings->verbose = 1;
                verbose("[+] verbose mode enabled\n");
                break;

            case 'x':
                settings->dump = 1;
                break;

            case 's':
                settings->sync = 1;
                break;

            case 'h':
                usage();

            case '?':
            default:
               exit(EXIT_FAILURE);
        }
    }

    if(!dir_exists(settings->datapath)) {
        verbose("[+] creating datapath: %s\n", settings->datapath);
        dir_create(settings->datapath);
    }

    if(!dir_exists(settings->indexpath)) {
        verbose("[+] creating indexpath: %s\n", settings->indexpath);
        dir_create(settings->indexpath);
    }

    return proceed(settings);
}
