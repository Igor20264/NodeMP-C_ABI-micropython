#ifndef NODEMP_NODE_INT_H
#define NODEMP_NODE_INT_H

#include <stddef.h>
#include <stdint.h>

#include "py/obj.h"
#include "node.h"

extern const NodeApi *g_api;

void nodemp_log(const char *msg);
void nodemp_log_error(const char *msg);
void nodemp_vm_lock(void);
void nodemp_vm_unlock(void);

int nodemp_current(void);
void nodemp_set_current(int index);

int nodemp_enqueue(int slot);
int nodemp_core_index(void);
int nodemp_processor_count(void);
int nodemp_reserve_flag(void);
int nodemp_pinned(void);
unsigned long nodemp_lane_tid(void);
int nodemp_on_lane(void);
void nodemp_reserve_now(void);
int nodemp_stopping(void);

void nodemp_require(size_t offset, const char *name);
void nodemp_store(mp_obj_dict_t *dict, const char *name, mp_obj_t value);
const char *nodemp_cstr(mp_obj_t obj);
int nodemp_api_has(size_t offset);

void nodemp_boot(void);
int nodemp_load(const char *name, const char *dir, const char *source, size_t length, const char *display_path);
void nodemp_unload(const char *name);
void nodemp_run_slot(int slot);
void nodemp_discard_slot(int slot);

void nodemp_register_auto(mp_obj_dict_t *dict);

#endif
