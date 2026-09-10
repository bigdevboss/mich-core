#ifndef MICH64_USER_MEMORY_H
#define MICH64_USER_MEMORY_H

int mich_page_create(void);
int mich_shared_memory_create(unsigned int pages);
int mich_page_map(unsigned int handle, unsigned long long virtual_address);
int mich_page_pin(unsigned int handle);
int mich_page_unpin(unsigned int handle);
int mich_page_revoke(unsigned int handle);

#endif
