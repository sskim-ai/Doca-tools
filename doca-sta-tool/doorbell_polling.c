#include <stdint.h>
#include <unistd.h>

void doorbell_polling_thread(
    volatile uint32_t *dummy_db,
    volatile uint32_t *real_db,
    const volatile int *stop_requested)
{
    uint32_t last = *dummy_db;

    while (!*stop_requested) {
        uint32_t cur = *dummy_db;

        if (cur != last) {
            *real_db = cur;
            last = cur;
        }

        usleep(1);
    }
}
