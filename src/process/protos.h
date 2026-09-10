#ifndef PROTOS_H
#define PROTOS_H

#define MSG_IRQ        1
#define KBD_SUBSCRIBE  2
#define KBD_EVENT      3
#define ATA_READ_REQ   4
#define ATA_CHUNK      5
#define ATA_ERR        6
#define ATA_WRITE_REQ  7
#define ATA_WRITTEN    8

#define FS_LIST        11
#define FS_DIRENT      12
#define FS_STAT        13
#define FS_STAT_RSP    14
#define FS_READ        15
#define FS_DATA        16
#define FS_ERR         17
#define FS_WRITE       18
#define FS_WRITTEN     19
#define FS_FORMAT      20
#define FS_FORMATTED   21

#define IPC_PING         30
#define IPC_PONG         31
#define IPC_STR          40
#define IPC_EOT          41
#define MSG_DIED         60

#define MFS_FORMAT_KEY 0x4D494348

#define ATA_CHUNKS  10
#define ATA_PAYLOAD 52
#define FS_PAYLOAD  52

#define EPERM         (-1)
#define ENOENT        (-2)
#define ESRCH         (-3)
#define ENOMEM       (-12)
#define EISDIR       (-21)
#define EINVAL       (-22)
#define EFBIG        (-27)
#define ENOSPC       (-28)
#define EDEADLK       (-35)
#define ENAMETOOLONG (-36)
#define ETIMEDOUT   (-110)

void panic(unsigned int vec, unsigned int err, unsigned int cr2,
           unsigned int eip) __attribute__((noreturn));
void panic_df(void) __attribute__((noreturn));
void panic_str(const char *msg) __attribute__((noreturn));

#endif
