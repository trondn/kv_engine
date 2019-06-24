#include <daemon/connection.h>
#include <daemon/cookie.h>
#include <daemon/front_end_thread.h>
#include <daemon/memcached.h>
#include <daemon/settings.h>
#include <daemon/stats.h>

#include <cstdio>

static void display(const char *name, size_t size) {
    printf("%s\t%d\n", name, (int)size);
}

static unsigned int count_used_opcodes() {
    unsigned int used_opcodes = 0;
    for (uint8_t opcode = 0; opcode < 255; opcode++) {
        try {
            to_string(cb::mcbp::Status(opcode));
            used_opcodes++;
        } catch (const std::exception&) {
        }
    }
    return used_opcodes;
}

static void display_used_opcodes(void) {
    printf("ClientOpcode map:     (# = Used, . = Free)\n\n");
    printf("   0123456789abcdef");
    for (unsigned int opcode = 0; opcode < 256; opcode++) {
        if (opcode % 16 == 0) {
            printf("\n%02x ", opcode & ~0xf);
        }
        try {
            to_string(cb::mcbp::Status(opcode));
            putchar('X');
        } catch (const std::exception&) {
            putchar('.');
        }
    }
    putchar('\n');
}

int main(int argc, char **argv) {
    display("Thread stats", sizeof(struct thread_stats));
    display("Global stats", sizeof(struct stats));
    display("Settings", sizeof(Settings));
    display("Libevent thread", sizeof(FrontEndThread));
    display("Connection", sizeof(Connection));

    printf("----------------------------------------\n");

    display("Thread stats cumulative\t", sizeof(struct thread_stats));
    printf("Binary protocol opcodes used\t%u / %u\n",
           count_used_opcodes(), 256);
    display_used_opcodes();

    return 0;
}
