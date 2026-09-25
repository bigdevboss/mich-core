#ifndef TEST_REPORT_H
#define TEST_REPORT_H

#include "types.h"

#define TEST_REPORT_MAX 64

#define TEST_ID_OBJECT 1
#define TEST_ID_VFS 2
#define TEST_ID_RESOURCE 3
#define TEST_ID_PAGE 4
#define TEST_ID_SG 5
#define TEST_ID_RING 6
#define TEST_ID_ASYNC 7
#define TEST_ID_BLOCK 8
#define TEST_ID_CACHE 9
#define TEST_ID_BLOCKFS 10
#define TEST_ID_VIRTIO_BLK 11
#define TEST_ID_POSIX_FD 12
#define TEST_ID_POSIX_PROFILE 13
#define TEST_ID_POSIX_VFS 14
#define TEST_ID_POSIX_PROCESS 15
#define TEST_ID_NET_FOUNDATION 16
#define TEST_ID_ETHERNET 17
#define TEST_ID_ARP 18
#define TEST_ID_IPV4 19
#define TEST_ID_IPV6 20
#define TEST_ID_ICMPV6 21
#define TEST_ID_UDPV6 22
#define TEST_ID_TCP 23
#define TEST_ID_ICMP 24
#define TEST_ID_LOOPBACK 25
#define TEST_ID_UDP 26
#define TEST_ID_SOCKET 27
#define TEST_ID_NET_REVOKE 28
#define TEST_ID_NET_INTERFACE 29
#define TEST_ID_NET_FUZZ 30
#define TEST_ID_NET_BENCH 31
#define TEST_ID_VFS_PAGES 32
#define TEST_ID_PAGE_GROW 33
#define TEST_ID_TCP_PAGES 34
#define TEST_ID_SOCKET_SEND_FILE 35
#define TEST_ID_SOCKET_RECEIVE_FILE 36
#define TEST_ID_NVME 37
#define TEST_ID_BLOCKFS_PAGES 38
#define TEST_ID_SOCKET_SEND_DISK_FILE 39
#define TEST_ID_DNS_MESSAGE 42
#define TEST_ID_DRIVER 40
#define TEST_ID_SUPERVISOR 41
#define TEST_ID_MSI 48
#define TEST_ID_MSIX 49
#define TEST_ID_VIRTIO 50
#define TEST_ID_IRQ 51
#define TEST_ID_IOREMAP 52
#define TEST_ID_FPU 60
#define TEST_ID_SMP_IPI 70
#define TEST_ID_SMP_SPIN 71
#define TEST_ID_SMP_TIMER 72
#define TEST_ID_SMP_TSS 73
#define TEST_ID_SMP_CURRENT 74
#define TEST_ID_SMP_SYSCALL 75
#define TEST_ID_SMP_USER 76
#define TEST_ID_SMP_USER_IRQ 77
#define TEST_ID_SMP_LIVE 78
#define TEST_ID_SMP_SCHED 79
#define TEST_ID_ENTROPY 80
#define TEST_ID_STREAM_ROUTE 81
#define TEST_ID_RTC 82
#define TEST_ID_SHA256 83
#define TEST_ID_AES_GCM 84
#define TEST_ID_X25519 85
#define TEST_ID_P256 86
#define TEST_ID_RSA 87
#define TEST_ID_X509 88

struct test_result64 {
    u32 id;
    i32 status;
};

void test_report_reset(void);
int test_report_record(u32 id, int status);
u32 test_report_count(void);
u32 test_report_failures(void);
void test_report_finish(void) __attribute__((noreturn));

#endif
