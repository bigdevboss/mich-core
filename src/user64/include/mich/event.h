#ifndef MICH64_USER_EVENT_H
#define MICH64_USER_EVENT_H

#define MICH_EVENT_AUTO_RESET 0
#define MICH_EVENT_MANUAL_RESET 1

#define MICH_RIGHT_READ (1u << 0)
#define MICH_RIGHT_WRITE (1u << 1)
#define MICH_RIGHT_WAIT (1u << 2)
#define MICH_RIGHT_SIGNAL (1u << 3)
#define MICH_RIGHT_MAP (1u << 4)
#define MICH_RIGHT_CONTROL (1u << 5)
#define MICH_RIGHT_TRANSFER (1u << 6)
#define MICH_HANDLE_TRANSFER_MAX 16

struct mich_handle_transfer {
    unsigned int source_handle;
    unsigned int rights;
    unsigned int target_handle;
};

int mich_handle_close(unsigned int handle);
int mich_handle_duplicate(int target_pid, unsigned int handle,
                          unsigned int rights);
int mich_handle_transfer_batch(int target_pid,
                               struct mich_handle_transfer *entries,
                               unsigned int count);
int mich_event_create(unsigned int mode, int signaled);
int mich_event_wait(unsigned int handle);
int mich_event_wait_timeout(unsigned int handle, unsigned int timeout_ticks);
int mich_event_signal(unsigned int handle);
int mich_event_reset(unsigned int handle);

#endif
