#ifndef NET_BUFFER_H
#define NET_BUFFER_H

#include "types.h"
#include "object.h"
#include "net_abi.h"

#define PACKET_POOL_MAX 16
#define PACKET_POOL_BUFFER_MAX 64

struct packet_pool_state;
void packet_pool_init(void);
struct kernel_object *packet_pool_create(u32 buffer_count);
struct kernel_object *packet_pool_backing(struct kernel_object *pool);
struct packet_pool_state *packet_pool_state_for(const struct kernel_object *pool);
u64 packet_pool_acquire(struct kernel_object *pool, u32 state);
u64 packet_pool_acquire_res(struct packet_pool_state *pool, u32 state);
int packet_pool_transition(struct kernel_object *pool, u64 id,
                           u32 expected_state, u32 new_state);
int packet_pool_transition_res(struct packet_pool_state *pool, u64 id,
                               u32 expected_state, u32 new_state);
int packet_pool_release(struct kernel_object *pool, u64 id,
                        u32 expected_state);
int packet_pool_release_res(struct packet_pool_state *pool, u64 id,
                            u32 expected_state);
void *packet_pool_data(struct kernel_object *pool, u64 id,
                       u32 required_state);
void *packet_pool_data_res(struct packet_pool_state *pool, u64 id,
                           u32 required_state);
int packet_pool_revoke(struct kernel_object *pool);
u32 packet_pool_free_count(struct kernel_object *pool);
u32 packet_pool_active_count(void);

#endif
