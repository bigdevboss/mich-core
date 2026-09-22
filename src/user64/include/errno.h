#ifndef MICH64_ERRNO_H
#define MICH64_ERRNO_H

extern int errno;

/* Reserved v0 errno surface of the POSIX application profile (4.7) plus
   ECHILD, which waitpid needs and which keeps its canonical value. */
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define E2BIG 7
#define EBADF 9
#define ECHILD 10
#define ENOMEM 12
#define EACCES 13
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define EOVERFLOW 75
#define EOPNOTSUPP 95
#define ETIMEDOUT 110

#endif
