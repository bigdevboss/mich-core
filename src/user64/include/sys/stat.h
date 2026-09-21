#ifndef MICH64_SYS_STAT_H
#define MICH64_SYS_STAT_H

#include <sys/types.h>

#define S_IFMT 0170000u
#define S_IFREG 0100000u
#define S_IFDIR 0040000u
#define S_IRUSR 0400u
#define S_IWUSR 0200u
#define S_IXUSR 0100u
#define S_IRGRP 0040u
#define S_IWGRP 0020u
#define S_IXGRP 0010u
#define S_IROTH 0004u
#define S_IWOTH 0002u
#define S_IXOTH 0001u

#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)

struct stat {
    unsigned int st_mode;
    unsigned int st_size;
    unsigned int st_nlink;
    unsigned int st_reserved;
};

int stat(const char *path, struct stat *buffer);
int fstat(int fd, struct stat *buffer);
int mkdir(const char *path, mode_t mode);
int rmdir(const char *path);
int unlink(const char *path);

#endif
