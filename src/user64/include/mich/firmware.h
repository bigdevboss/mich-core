#ifndef MICH64_USER_FIRMWARE_H
#define MICH64_USER_FIRMWARE_H

#include <firmware_abi.h>

#define MICH_FIRMWARE_NAME_MAX FIRMWARE_NAME_MAX
#define mich_firmware_open_request firmware_open_request

int mich_firmware_open(struct mich_firmware_open_request *request);

#endif
