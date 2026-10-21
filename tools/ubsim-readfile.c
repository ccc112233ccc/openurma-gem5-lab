/* Minimal gem5 readfile bootstrap for checkpointed guests without m5 CLI. */
#include <gem5/m5ops.h>
#include <stdint.h>
#include <unistd.h>

int main(void)
{
    char buffer[4096];
    uint64_t offset = 0;
    for (;;) {
        uint64_t length = m5_read_file(buffer, sizeof(buffer), offset);
        if (length == 0)
            return 0;
        for (uint64_t done = 0; done < length;) {
            ssize_t written = write(STDOUT_FILENO, buffer + done, length - done);
            if (written <= 0)
                return 1;
            done += (uint64_t)written;
        }
        offset += length;
    }
}
