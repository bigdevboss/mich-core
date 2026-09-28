#ifndef MICH64_USER_BLOCK_H
#define MICH64_USER_BLOCK_H

#include <block_abi.h>
#include <block_driver_abi.h>

#define mich_block_driver_request block_driver_request
#define mich_block_driver_completion block_driver_completion
#define mich_block_driver_register_request block_driver_register_request

#define MICH_BLOCK_SECTOR_SIZE BLOCK_SECTOR_SIZE
#define MICH_BLOCK_OP_READ BLOCK_OP_READ
#define MICH_BLOCK_OP_WRITE BLOCK_OP_WRITE
#define MICH_BLOCK_FLAG_READ_ONLY BLOCK_FLAG_READ_ONLY
#define MICH_BLOCK_FLAG_DEFER BLOCK_FLAG_DEFER

#define mich_block_io_request block_io_request

int mich_block_create(unsigned int sector_count, unsigned int flags);
int mich_block_info(int handle, struct block_info *info);
int mich_block_submit(struct mich_block_io_request *request);
int mich_block_collect(struct mich_block_io_request *request);
int mich_block_revoke(int handle);
int mich_block_service(int handle);
int mich_block_interface_create(struct mich_block_driver_register_request *request);
int mich_nvme_open(int pci_handle);

#endif
