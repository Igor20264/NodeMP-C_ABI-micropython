#include <stdlib.h>
#include <string.h>

#include "node_int.h"

#include "py/compile.h"
#include "py/lexer.h"
#include "py/objtuple.h"
#include "py/parse.h"
#include "py/runtime.h"

static __thread int g_current = -1;

typedef struct Slot {
    int kind;
    int res;
    int timer_id;
    char *name;
} Slot;

typedef struct Res {
    int used;
    int alive;
    int trusted;
    char name[128];
    char dir[512];
} Res;

enum {
    KIND_FREE = 0,
    KIND_CLIENT,
    KIND_BUILTIN,
    KIND_VEHICLE,
    KIND_REQUEST,
    KIND_TIMEOUT,
    KIND_INTERVAL,
    KIND_JOB
};

static Slot g_slot[NODEMP_SLOTS];
static Res g_res[NODEMP_RESOURCES];
static mp_obj_full_type_t g_node_type;
static mp_obj_base_t g_node_obj;
static qstr g_q_resource_name;
static qstr g_q_resource_dir;
static qstr g_q_trusted;

static const char *const REQUESTS[] = {
    "playerConnectRequest", "vehicleSpawnRequest", "vehicleEnterRequest", "vehicleExitRequest",
    "vehicleCouplerRequest", "vehicleEditRequest", "vehiclePaintRequest", "vehicleTriggerRequest",
    "vehicleNodeGrabRequest", "queueJoinRequest", "queueAdmitRequest", "chatMessageRequest", NULL,
};

static const char *const VEHICLE[] = {
    "vehicleEdited", "vehicleReset", "vehiclePainted", "playerSeatChanged", "vehicleCouplerChanged",
    "vehicleControllerChanged", "playerVerifyReported", "playerLagging", "playerRecovered",
    "vehicleDeleted", "playerLeft", "resourceUnload", "queueAdmitted", "queueLeft", "queueNoShow",
    "playerJoinSlow", "serverShutdownScheduled", "playerPolicyChanged", "playerFrozenChanged", NULL,
};

static const char *const BUILTIN[] = {
    "playerAuthenticated", "playerJoined", "playerLeft", "serverTick", "serverShutdown",
    "vehicleSpawned", "vehicleDeleted", "vehicleDamageChanged", "vehicleTagsChanged",
    "vehicleLockChanged", "vehiclePositionChanged", "vehicleInputsChanged", "vehicleElectricsChanged",
    "vehiclePowertrainChanged", "vehicleEngineChanged", "vehicleNodesChanged", "playerInputsChanged",
    "playerHeadPoseChanged", "vehicleTeleported", "vehicleBreakGroupsChanged", "playerCameraChanged", NULL,
};

int nodemp_current(void) { return g_current; }
void nodemp_set_current(int index) { g_current = index; }

int nodemp_api_has(size_t offset) {
    return g_api && (size_t)g_api->struct_size >= offset + sizeof(void *);
}

void nodemp_require(size_t offset, const char *name) {
    if (!nodemp_api_has(offset)) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s is not in this server"), name);
    }
}

const char *nodemp_cstr(mp_obj_t obj) {
    if (obj == mp_const_none) return NULL;
    return mp_obj_str_get_str(obj);
}

void nodemp_store(mp_obj_dict_t *dict, const char *name, mp_obj_t value) {
    mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(qstr_from_str(name)), value);
}

static int named(const char *name, const char *const *table) {
    int i;
    for (i = 0; table[i]; i++) {
        if (strcmp(name, table[i]) == 0) return 1;
    }
    return 0;
}

static void clear_roots(int slot) {
    MP_STATE_VM(nodemp_slot_fn)[slot] = MP_OBJ_NULL;
    MP_STATE_VM(nodemp_slot_args)[slot] = MP_OBJ_NULL;
    MP_STATE_VM(nodemp_slot_done)[slot] = MP_OBJ_NULL;
    MP_STATE_VM(nodemp_slot_result)[slot] = MP_OBJ_NULL;
    MP_STATE_VM(nodemp_slot_error)[slot] = MP_OBJ_NULL;
}

static void clear_slot(int slot) {
    free(g_slot[slot].name);
    g_slot[slot].name = NULL;
    g_slot[slot].kind = KIND_FREE;
    g_slot[slot].res = -1;
    g_slot[slot].timer_id = -1;
    clear_roots(slot);
}

static int alloc_slot(int kind, int res) {
    int i;
    for (i = 0; i < NODEMP_SLOTS; i++) {
        if (g_slot[i].kind != KIND_FREE) continue;
        g_slot[i].kind = kind;
        g_slot[i].res = res;
        g_slot[i].timer_id = -1;
        g_slot[i].name = NULL;
        clear_roots(i);
        return i;
    }
    return -1;
}

static void log_exc(mp_obj_t exc) {
    vstr_t vstr;
    mp_print_t print;
    char buf[1024];
    size_t n;
    vstr_init(&vstr, 80);
    print.data = &vstr;
    print.print_strn = (mp_print_strn_t)vstr_add_strn;
    mp_obj_print_exception(&print, exc);
    n = vstr.len < sizeof buf - 1 ? vstr.len : sizeof buf - 1;
    if (vstr.buf && n) memcpy(buf, vstr.buf, n);
    buf[n] = '\0';
    vstr_clear(&vstr);
    nodemp_log_error(buf[0] ? buf : "micropython exception");
}

static void owner_begin(int res) {
    if (res < 0 || !g_res[res].used) return;
    if (!nodemp_api_has(offsetof(NodeApi, set_resource_owner))) return;
    g_api->set_resource_owner(g_res[res].name);
}

static void owner_end(int res) {
    if (res < 0 || !g_res[res].used) return;
    if (!nodemp_api_has(offsetof(NodeApi, set_resource_owner))) return;
    g_api->set_resource_owner(NULL);
}

static mp_obj_t call_slot(int slot, size_t n_args, const mp_obj_t *args, int *failed) {
    nlr_buf_t nlr;
    mp_obj_t fn = MP_STATE_VM(nodemp_slot_fn)[slot];
    int prev = g_current;
    *failed = 0;
    g_current = g_slot[slot].res;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t ret = mp_call_function_n_kw(fn, n_args, 0, args);
        nlr_pop();
        g_current = prev;
        return ret;
    }
    g_current = prev;
    *failed = 1;
    log_exc(MP_OBJ_FROM_PTR(nlr.ret_val));
    return MP_OBJ_NULL;
}

static int slot_live(int slot) {
    int res;
    if (slot < 0 || slot >= NODEMP_SLOTS) return 0;
    if (g_slot[slot].kind == KIND_FREE) return 0;
    res = g_slot[slot].res;
    return res >= 0 && g_res[res].used && g_res[res].alive;
}

static void write_reason(char *reason, int cap, const char *text) {
    size_t n;
    if (!reason || cap <= 1 || !text) return;
    n = strlen(text);
    if (n >= (size_t)cap) n = (size_t)cap - 1;
    memcpy(reason, text, n);
    reason[n] = '\0';
}

static void on_client(int player_id, const char *data, void *user) {
    int slot = (int)(intptr_t)user;
    mp_obj_t args[2];
    int failed;
    nodemp_vm_lock();
    if (!slot_live(slot)) {
        nodemp_vm_unlock();
        return;
    }
    args[0] = mp_obj_new_int(player_id);
    args[1] = mp_obj_new_str(data ? data : "", data ? strlen(data) : 0);
    call_slot(slot, 2, args, &failed);
    nodemp_vm_unlock();
}

static void on_builtin(int player_id, void *user) {
    int slot = (int)(intptr_t)user;
    mp_obj_t arg;
    int failed;
    nodemp_vm_lock();
    if (!slot_live(slot)) {
        nodemp_vm_unlock();
        return;
    }
    arg = mp_obj_new_int(player_id);
    call_slot(slot, 1, &arg, &failed);
    nodemp_vm_unlock();
}

static void on_vehicle(int player_id, int global_id, const char *data, void *user) {
    int slot = (int)(intptr_t)user;
    mp_obj_t args[3];
    int failed;
    nodemp_vm_lock();
    if (!slot_live(slot)) {
        nodemp_vm_unlock();
        return;
    }
    args[0] = mp_obj_new_int(player_id);
    args[1] = mp_obj_new_int(global_id);
    args[2] = mp_obj_new_str(data ? data : "", data ? strlen(data) : 0);
    call_slot(slot, 3, args, &failed);
    nodemp_vm_unlock();
}

static int on_request(int player_id, int global_id, const char *data, char *reason, int reason_cap, void *user) {
    int slot = (int)(intptr_t)user;
    mp_obj_t args[3];
    mp_obj_t ret;
    int failed;
    nodemp_vm_lock();
    if (!slot_live(slot)) {
        nodemp_vm_unlock();
        return 0;
    }
    args[0] = mp_obj_new_int(player_id);
    args[1] = mp_obj_new_int(global_id);
    args[2] = mp_obj_new_str(data ? data : "", data ? strlen(data) : 0);
    ret = call_slot(slot, 3, args, &failed);
    nodemp_vm_unlock();
    if (failed || ret == mp_const_false) return 1;
    if (ret != MP_OBJ_NULL && mp_obj_is_str(ret)) {
        write_reason(reason, reason_cap, mp_obj_str_get_str(ret));
        return 1;
    }
    return 0;
}

static void on_timer(int timer_id, void *user) {
    int slot = (int)(intptr_t)user;
    mp_obj_t arg;
    int failed;
    int once;
    nodemp_vm_lock();
    if (!slot_live(slot)) {
        nodemp_vm_unlock();
        return;
    }
    once = g_slot[slot].kind == KIND_TIMEOUT;
    arg = mp_obj_new_int(timer_id);
    call_slot(slot, 1, &arg, &failed);
    if (once) clear_slot(slot);
    nodemp_vm_unlock();
}

static void finish_job(int timer_id, void *user) {
    int slot = (int)(intptr_t)user;
    mp_obj_t args[2];
    mp_obj_t done;
    (void)timer_id;
    nodemp_vm_lock();
    if (slot < 0 || slot >= NODEMP_SLOTS || g_slot[slot].kind != KIND_JOB) {
        nodemp_vm_unlock();
        return;
    }
    if (!slot_live(slot)) {
        clear_slot(slot);
        nodemp_vm_unlock();
        return;
    }
    done = MP_STATE_VM(nodemp_slot_done)[slot];
    args[0] = MP_STATE_VM(nodemp_slot_result)[slot];
    args[1] = MP_STATE_VM(nodemp_slot_error)[slot];
    if (args[0] == MP_OBJ_NULL) args[0] = mp_const_none;
    if (args[1] == MP_OBJ_NULL) args[1] = mp_const_none;
    if (done != MP_OBJ_NULL && done != mp_const_none) {
        nlr_buf_t nlr;
        int prev = g_current;
        g_current = g_slot[slot].res;
        if (nlr_push(&nlr) == 0) {
            mp_call_function_n_kw(done, 2, 0, args);
            nlr_pop();
        } else {
            log_exc(MP_OBJ_FROM_PTR(nlr.ret_val));
        }
        g_current = prev;
    } else if (args[1] != mp_const_none) {
        nodemp_log_error(mp_obj_str_get_str(args[1]));
    }
    clear_slot(slot);
    nodemp_vm_unlock();
}

void nodemp_discard_slot(int slot) {
    if (slot < 0 || slot >= NODEMP_SLOTS) return;
    if (g_slot[slot].kind == KIND_JOB) clear_slot(slot);
}

void nodemp_run_slot(int slot) {
    mp_obj_t fn;
    mp_obj_t args_obj;
    mp_obj_t result = mp_const_none;
    mp_obj_t error = mp_const_none;
    size_t n = 0;
    mp_obj_t *items = NULL;
    nlr_buf_t nlr;
    int prev;
    int scheduled = 0;
    nodemp_vm_lock();
    if (slot < 0 || slot >= NODEMP_SLOTS || g_slot[slot].kind != KIND_JOB) {
        nodemp_vm_unlock();
        return;
    }
    if (!slot_live(slot) || nodemp_stopping()) {
        clear_slot(slot);
        nodemp_vm_unlock();
        return;
    }
    fn = MP_STATE_VM(nodemp_slot_fn)[slot];
    args_obj = MP_STATE_VM(nodemp_slot_args)[slot];
    if (args_obj != MP_OBJ_NULL) mp_obj_tuple_get(args_obj, &n, &items);
    prev = g_current;
    g_current = g_slot[slot].res;
    if (nlr_push(&nlr) == 0) {
        result = mp_call_function_n_kw(fn, n, 0, items);
        nlr_pop();
    } else {
        vstr_t vstr;
        mp_print_t print;
        vstr_init(&vstr, 80);
        print.data = &vstr;
        print.print_strn = (mp_print_strn_t)vstr_add_strn;
        mp_obj_print_exception(&print, MP_OBJ_FROM_PTR(nlr.ret_val));
        error = mp_obj_new_str(vstr.buf ? vstr.buf : "", vstr.len);
        vstr_clear(&vstr);
        result = mp_const_none;
    }
    g_current = prev;
    MP_STATE_VM(nodemp_slot_result)[slot] = result;
    MP_STATE_VM(nodemp_slot_error)[slot] = error;
    if (nodemp_api_has(offsetof(NodeApi, set_timeout)) && !nodemp_stopping()) {
        if (g_api->set_timeout(0, finish_job, (void *)(intptr_t)slot) >= 0) scheduled = 1;
    }
    if (!scheduled) {
        if (error != mp_const_none) nodemp_log_error(mp_obj_str_get_str(error));
        clear_slot(slot);
    }
    nodemp_vm_unlock();
}

static int bind_event(int kind, const char *name, mp_obj_t fn) {
    int res = g_current;
    int slot;
    char *kept;
    if (!mp_obj_is_callable(fn)) mp_raise_TypeError(MP_ERROR_TEXT("handler must be callable"));
    if (!name) mp_raise_ValueError(MP_ERROR_TEXT("event name is missing"));
    slot = alloc_slot(kind, res);
    if (slot < 0) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("too many micropython handlers"));
    kept = (char *)malloc(strlen(name) + 1);
    if (!kept) {
        clear_slot(slot);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("out of memory"));
    }
    memcpy(kept, name, strlen(name) + 1);
    g_slot[slot].name = kept;
    MP_STATE_VM(nodemp_slot_fn)[slot] = fn;
    owner_begin(res);
    if (kind == KIND_CLIENT) g_api->register_client_event(kept, on_client, (void *)(intptr_t)slot);
    else if (kind == KIND_BUILTIN) g_api->register_builtin_event(kept, on_builtin, (void *)(intptr_t)slot);
    else if (kind == KIND_VEHICLE) g_api->register_vehicle_event(kept, on_vehicle, (void *)(intptr_t)slot);
    else g_api->register_cancellable_event(kept, on_request, (void *)(intptr_t)slot);
    owner_end(res);
    return slot;
}

static mp_obj_t node_on(mp_obj_t name_obj, mp_obj_t fn) {
    const char *name = mp_obj_str_get_str(name_obj);
    int kind;
    int slot;
    if (named(name, REQUESTS)) kind = KIND_REQUEST;
    else if (named(name, VEHICLE)) kind = KIND_VEHICLE;
    else if (named(name, BUILTIN)) kind = KIND_BUILTIN;
    else kind = KIND_CLIENT;
    nodemp_require(offsetof(NodeApi, register_client_event), "register_client_event");
    slot = bind_event(kind, name, fn);
    return mp_obj_new_int(slot);
}
static MP_DEFINE_CONST_FUN_OBJ_2(node_on_obj, node_on);

static void unbind(int slot) {
    int kind = g_slot[slot].kind;
    void *user = (void *)(intptr_t)slot;
    const char *name = g_slot[slot].name;
    if (kind == KIND_CLIENT) g_api->unregister_client_event(name, on_client, user);
    else if (kind == KIND_BUILTIN) g_api->unregister_builtin_event(name, on_builtin, user);
    else if (kind == KIND_VEHICLE) g_api->unregister_vehicle_event(name, on_vehicle, user);
    else if (kind == KIND_REQUEST) g_api->unregister_cancellable_event(name, on_request, user);
    else if ((kind == KIND_TIMEOUT || kind == KIND_INTERVAL) && nodemp_api_has(offsetof(NodeApi, clear_timer))) {
        g_api->clear_timer(g_slot[slot].timer_id);
    }
}

static mp_obj_t node_off(mp_obj_t handle) {
    int slot = mp_obj_get_int(handle);
    if (slot < 0 || slot >= NODEMP_SLOTS || g_slot[slot].kind == KIND_FREE || g_slot[slot].kind == KIND_JOB) {
        return mp_const_none;
    }
    if (g_slot[slot].kind == KIND_TIMEOUT || g_slot[slot].kind == KIND_INTERVAL) {
        return mp_const_none;
    }
    unbind(slot);
    clear_slot(slot);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_off_obj, node_off);

static mp_obj_t start_timer(mp_obj_t ms_obj, mp_obj_t fn, int interval) {
    int res = g_current;
    int slot;
    int timer_id;
    uint32_t ms;
    if (!mp_obj_is_callable(fn)) mp_raise_TypeError(MP_ERROR_TEXT("timer function must be callable"));
    if (interval) nodemp_require(offsetof(NodeApi, set_interval), "set_interval");
    else nodemp_require(offsetof(NodeApi, set_timeout), "set_timeout");
    ms = (uint32_t)mp_obj_get_int(ms_obj);
    slot = alloc_slot(interval ? KIND_INTERVAL : KIND_TIMEOUT, res);
    if (slot < 0) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("too many micropython handlers"));
    MP_STATE_VM(nodemp_slot_fn)[slot] = fn;
    owner_begin(res);
    timer_id = interval ? g_api->set_interval(ms, on_timer, (void *)(intptr_t)slot)
                        : g_api->set_timeout(ms, on_timer, (void *)(intptr_t)slot);
    owner_end(res);
    if (timer_id < 0) {
        clear_slot(slot);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("timer registration failed"));
    }
    g_slot[slot].timer_id = timer_id;
    return mp_obj_new_int(timer_id);
}

static mp_obj_t node_set_timeout(mp_obj_t ms, mp_obj_t fn) { return start_timer(ms, fn, 0); }
static mp_obj_t node_set_interval(mp_obj_t ms, mp_obj_t fn) { return start_timer(ms, fn, 1); }
static MP_DEFINE_CONST_FUN_OBJ_2(node_set_timeout_obj, node_set_timeout);
static MP_DEFINE_CONST_FUN_OBJ_2(node_set_interval_obj, node_set_interval);

static mp_obj_t node_cancel(mp_obj_t handle) {
    int timer_id = mp_obj_get_int(handle);
    int i;
    if (nodemp_api_has(offsetof(NodeApi, clear_timer))) g_api->clear_timer(timer_id);
    for (i = 0; i < NODEMP_SLOTS; i++) {
        if ((g_slot[i].kind == KIND_TIMEOUT || g_slot[i].kind == KIND_INTERVAL) && g_slot[i].timer_id == timer_id) {
            clear_slot(i);
            break;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_cancel_obj, node_cancel);

static mp_obj_t node_job(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    int res = g_current;
    int slot;
    mp_obj_t fn = pos_args[0];
    mp_obj_t done = mp_const_none;
    mp_obj_t args;
    mp_map_elem_t *elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(qstr_from_str("done")), MP_MAP_LOOKUP);
    if (!mp_obj_is_callable(fn)) mp_raise_TypeError(MP_ERROR_TEXT("job function must be callable"));
    if (elem && elem->value != MP_OBJ_NULL) {
        done = elem->value;
        if (done != mp_const_none && !mp_obj_is_callable(done)) {
            mp_raise_TypeError(MP_ERROR_TEXT("done must be callable"));
        }
    }
    slot = alloc_slot(KIND_JOB, res);
    if (slot < 0) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("too many micropython jobs"));
    args = mp_obj_new_tuple(n_args - 1, (mp_obj_t *)(pos_args + 1));
    MP_STATE_VM(nodemp_slot_fn)[slot] = fn;
    MP_STATE_VM(nodemp_slot_args)[slot] = args;
    MP_STATE_VM(nodemp_slot_done)[slot] = done;
    if (nodemp_enqueue(slot) != 0) {
        clear_slot(slot);
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("micropython core lane rejected the job"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(node_job_obj, 1, node_job);

static mp_obj_t node_core_info(void) {
    mp_obj_t dict = mp_obj_new_dict(5);
    nodemp_store(MP_OBJ_TO_PTR(dict), "index", mp_obj_new_int(nodemp_core_index()));
    nodemp_store(MP_OBJ_TO_PTR(dict), "processors", mp_obj_new_int(nodemp_processor_count()));
    nodemp_store(MP_OBJ_TO_PTR(dict), "reserved", mp_obj_new_int(nodemp_reserve_flag()));
    nodemp_store(MP_OBJ_TO_PTR(dict), "pinned", mp_obj_new_bool(nodemp_pinned()));
    nodemp_store(MP_OBJ_TO_PTR(dict), "lane_id", mp_obj_new_int_from_uint(nodemp_lane_tid()));
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(node_core_info_obj, node_core_info);

static mp_obj_t node_on_core(void) { return mp_obj_new_bool(nodemp_on_lane()); }
static MP_DEFINE_CONST_FUN_OBJ_0(node_on_core_obj, node_on_core);

static mp_obj_t node_reserve_core(void) {
    nodemp_reserve_now();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(node_reserve_core_obj, node_reserve_core);

static mp_obj_t id_list(int (*fn)(int *, int), int cap) {
    if (cap < 1) cap = 1;
    for (;;) {
        int *ids = (int *)malloc(sizeof(int) * (size_t)cap);
        int count;
        int i;
        mp_obj_t list;
        if (!ids) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("out of memory"));
        count = fn(ids, cap);
        if (count < 0) {
            free(ids);
            return mp_obj_new_list(0, NULL);
        }
        if (count < cap || cap >= 100000) {
            list = mp_obj_new_list(0, NULL);
            for (i = 0; i < count; i++) mp_obj_list_append(list, mp_obj_new_int(ids[i]));
            free(ids);
            return list;
        }
        free(ids);
        cap *= 2;
    }
}

static int players_fn(int *ids, int max) { return g_api->get_players(ids, max); }
static int vehicles_fn(int *ids, int max) { return g_api->get_vehicles(ids, max); }

static mp_obj_t node_get_players(void) {
    int cap = 1;
    nodemp_require(offsetof(NodeApi, get_players), "get_players");
    if (nodemp_api_has(offsetof(NodeApi, get_player_count))) {
        int n = g_api->get_player_count();
        if (n > cap) cap = n;
    }
    return id_list(players_fn, cap);
}
static MP_DEFINE_CONST_FUN_OBJ_0(node_get_players_obj, node_get_players);

static mp_obj_t node_get_vehicles(void) {
    int cap = 1;
    nodemp_require(offsetof(NodeApi, get_vehicles), "get_vehicles");
    if (nodemp_api_has(offsetof(NodeApi, get_vehicle_count))) cap = g_api->get_vehicle_count();
    if (cap < 1) cap = 1;
    return id_list(vehicles_fn, cap);
}
static MP_DEFINE_CONST_FUN_OBJ_0(node_get_vehicles_obj, node_get_vehicles);

static int g_player_for_list;

static int player_vehicles_fn(int *ids, int max) {
    return g_api->get_player_vehicles(g_player_for_list, ids, max);
}

static mp_obj_t node_get_player_vehicles(mp_obj_t player) {
    nodemp_require(offsetof(NodeApi, get_player_vehicles), "get_player_vehicles");
    g_player_for_list = mp_obj_get_int(player);
    return id_list(player_vehicles_fn, 8);
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_get_player_vehicles_obj, node_get_player_vehicles);

static mp_obj_t node_get_player_vehicle(mp_obj_t player) {
    int role = 0;
    int gid;
    mp_obj_t items[2];
    nodemp_require(offsetof(NodeApi, get_player_vehicle), "get_player_vehicle");
    gid = g_api->get_player_vehicle(mp_obj_get_int(player), &role);
    items[0] = mp_obj_new_int(gid);
    items[1] = mp_obj_new_int(role);
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_get_player_vehicle_obj, node_get_player_vehicle);

static const char *g_tag_key;
static const char *g_tag_value;

static int tag_fn(int *ids, int max) {
    return g_api->get_vehicles_by_tag(g_tag_key, g_tag_value, ids, max);
}

static mp_obj_t node_get_vehicles_by_tag(size_t n_args, const mp_obj_t *args) {
    nodemp_require(offsetof(NodeApi, get_vehicles_by_tag), "get_vehicles_by_tag");
    g_tag_key = nodemp_cstr(args[0]);
    g_tag_value = n_args > 1 ? nodemp_cstr(args[1]) : NULL;
    return id_list(tag_fn, 16);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(node_get_vehicles_by_tag_obj, 1, 2, node_get_vehicles_by_tag);

static mp_obj_t node_get_vehicles_by_tags(mp_obj_t pairs) {
    const char *json = nodemp_cstr(pairs);
    int cap = 16;
    nodemp_require(offsetof(NodeApi, get_vehicles_by_tags), "get_vehicles_by_tags");
    for (;;) {
        int *ids = (int *)malloc(sizeof(int) * (size_t)cap);
        int count;
        int i;
        mp_obj_t list;
        if (!ids) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("out of memory"));
        count = g_api->get_vehicles_by_tags(json, ids, cap);
        if (count < 0) {
            free(ids);
            return mp_obj_new_list(0, NULL);
        }
        if (count < cap || cap >= 100000) {
            list = mp_obj_new_list(0, NULL);
            for (i = 0; i < count; i++) mp_obj_list_append(list, mp_obj_new_int(ids[i]));
            free(ids);
            return list;
        }
        free(ids);
        cap *= 2;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_1(node_get_vehicles_by_tags_obj, node_get_vehicles_by_tags);

static void node_attr(mp_obj_t self, qstr attr, mp_obj_t *dest) {
    mp_obj_dict_t *methods;
    mp_map_elem_t *elem;
    (void)self;
    if (dest[0] != MP_OBJ_NULL) return;
    if (attr == g_q_resource_name) {
        if (g_current < 0 || !g_res[g_current].used) dest[0] = mp_const_none;
        else dest[0] = mp_obj_new_str(g_res[g_current].name, strlen(g_res[g_current].name));
        return;
    }
    if (attr == g_q_resource_dir) {
        if (g_current < 0 || !g_res[g_current].used) dest[0] = mp_const_none;
        else dest[0] = mp_obj_new_str(g_res[g_current].dir, strlen(g_res[g_current].dir));
        return;
    }
    if (attr == g_q_trusted) {
        dest[0] = (g_current >= 0 && g_res[g_current].used && g_res[g_current].trusted && g_res[g_current].alive)
            ? mp_const_true : mp_const_false;
        return;
    }
    methods = MP_OBJ_TO_PTR(MP_STATE_VM(nodemp_methods));
    if (!methods) return;
    elem = mp_map_lookup(&methods->map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
    if (elem && elem->value != MP_OBJ_NULL) dest[0] = elem->value;
}

static void register_manual(mp_obj_dict_t *dict) {
    nodemp_store(dict, "on", MP_OBJ_FROM_PTR(&node_on_obj));
    nodemp_store(dict, "off", MP_OBJ_FROM_PTR(&node_off_obj));
    nodemp_store(dict, "set_timeout", MP_OBJ_FROM_PTR(&node_set_timeout_obj));
    nodemp_store(dict, "set_interval", MP_OBJ_FROM_PTR(&node_set_interval_obj));
    nodemp_store(dict, "after", MP_OBJ_FROM_PTR(&node_set_timeout_obj));
    nodemp_store(dict, "every", MP_OBJ_FROM_PTR(&node_set_interval_obj));
    nodemp_store(dict, "cancel", MP_OBJ_FROM_PTR(&node_cancel_obj));
    nodemp_store(dict, "job", MP_OBJ_FROM_PTR(&node_job_obj));
    nodemp_store(dict, "core_info", MP_OBJ_FROM_PTR(&node_core_info_obj));
    nodemp_store(dict, "on_core", MP_OBJ_FROM_PTR(&node_on_core_obj));
    nodemp_store(dict, "reserve_core", MP_OBJ_FROM_PTR(&node_reserve_core_obj));
    nodemp_store(dict, "get_players", MP_OBJ_FROM_PTR(&node_get_players_obj));
    nodemp_store(dict, "get_vehicles", MP_OBJ_FROM_PTR(&node_get_vehicles_obj));
    nodemp_store(dict, "get_player_vehicles", MP_OBJ_FROM_PTR(&node_get_player_vehicles_obj));
    nodemp_store(dict, "get_player_vehicle", MP_OBJ_FROM_PTR(&node_get_player_vehicle_obj));
    nodemp_store(dict, "get_vehicles_by_tag", MP_OBJ_FROM_PTR(&node_get_vehicles_by_tag_obj));
    nodemp_store(dict, "get_vehicles_by_tags", MP_OBJ_FROM_PTR(&node_get_vehicles_by_tags_obj));
}

void nodemp_boot(void) {
    mp_obj_t methods;
    memset(&g_node_type, 0, sizeof g_node_type);
    g_node_type.base.type = &mp_type_type;
    g_node_type.name = qstr_from_str("Node");
    MP_OBJ_TYPE_SET_SLOT((mp_obj_type_t *)&g_node_type, attr, node_attr, 0);
    g_node_obj.type = (const mp_obj_type_t *)&g_node_type;
    g_q_resource_name = qstr_from_str("resource_name");
    g_q_resource_dir = qstr_from_str("resource_dir");
    g_q_trusted = qstr_from_str("trusted");
    methods = mp_obj_new_dict(160);
    MP_STATE_VM(nodemp_methods) = methods;
    nodemp_register_auto(MP_OBJ_TO_PTR(methods));
    register_manual(MP_OBJ_TO_PTR(methods));
}

static int find_res(const char *name) {
    int i;
    for (i = 0; i < NODEMP_RESOURCES; i++) {
        if (g_res[i].used && strcmp(g_res[i].name, name) == 0) return i;
    }
    return -1;
}

static int alloc_res(const char *name, const char *dir) {
    int i;
    if (strlen(name) >= sizeof g_res[0].name || strlen(dir) >= sizeof g_res[0].dir) return -1;
    for (i = 0; i < NODEMP_RESOURCES; i++) {
        if (g_res[i].used) continue;
        memset(&g_res[i], 0, sizeof g_res[i]);
        g_res[i].used = 1;
        g_res[i].alive = 1;
        memcpy(g_res[i].name, name, strlen(name) + 1);
        memcpy(g_res[i].dir, dir, strlen(dir) + 1);
        return i;
    }
    return -1;
}

static void drop_res_slots(int res) {
    int i;
    for (i = 0; i < NODEMP_SLOTS; i++) {
        if (g_slot[i].res != res || g_slot[i].kind == KIND_FREE) continue;
        if (g_slot[i].kind == KIND_JOB) continue;
        unbind(i);
        clear_slot(i);
    }
}

int nodemp_load(const char *name, const char *dir, const char *source, size_t length, const char *display_path) {
    int index;
    int prev;
    mp_obj_dict_t *old_g;
    mp_obj_dict_t *old_l;
    mp_obj_t globals;
    nlr_buf_t nlr;
    if (!name || !name[0] || !source) return -1;
    if (find_res(name) >= 0) nodemp_unload(name);
    index = alloc_res(name, dir ? dir : "");
    if (index < 0) {
        nodemp_log_error("micropython resource table is full");
        return -1;
    }
    if (nodemp_api_has(offsetof(NodeApi, is_resource_trusted))) {
        g_res[index].trusted = g_api->is_resource_trusted(name) == 1;
    }
    old_g = mp_globals_get();
    old_l = mp_locals_get();
    prev = g_current;
    g_current = index;
    globals = mp_obj_new_dict(8);
    MP_STATE_VM(nodemp_resource_globals)[index] = globals;
    mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(qstr_from_str("__name__")), mp_obj_new_str(name, strlen(name)));
    if (display_path) {
        mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(qstr_from_str("__file__")), mp_obj_new_str(display_path, strlen(display_path)));
    }
    mp_obj_dict_store(globals, MP_OBJ_NEW_QSTR(qstr_from_str("node")), MP_OBJ_FROM_PTR(&g_node_obj));
    mp_globals_set(MP_OBJ_TO_PTR(globals));
    mp_locals_set(MP_OBJ_TO_PTR(globals));
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(qstr_from_str(display_path ? display_path : name), source, length, 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t fun = mp_compile(&tree, source_name, false);
        mp_call_function_0(fun);
        nlr_pop();
        mp_globals_set(old_g);
        mp_locals_set(old_l);
        g_current = prev;
        return 0;
    }
    log_exc(MP_OBJ_FROM_PTR(nlr.ret_val));
    mp_globals_set(old_g);
    mp_locals_set(old_l);
    g_res[index].alive = 0;
    drop_res_slots(index);
    MP_STATE_VM(nodemp_resource_globals)[index] = MP_OBJ_NULL;
    g_res[index].used = 0;
    g_current = prev;
    nodemp_log_error("micropython resource failed to load");
    return -1;
}

void nodemp_unload(const char *name) {
    int index = find_res(name ? name : "");
    mp_obj_t globals;
    mp_map_elem_t *elem;
    int prev;
    if (index < 0 || !g_res[index].alive) return;
    prev = g_current;
    g_current = index;
    globals = MP_STATE_VM(nodemp_resource_globals)[index];
    if (globals != MP_OBJ_NULL) {
        elem = mp_map_lookup(&((mp_obj_dict_t *)MP_OBJ_TO_PTR(globals))->map,
            MP_OBJ_NEW_QSTR(qstr_from_str("unload")), MP_MAP_LOOKUP);
        if (elem && elem->value != MP_OBJ_NULL && mp_obj_is_callable(elem->value)) {
            nlr_buf_t nlr;
            if (nlr_push(&nlr) == 0) {
                mp_call_function_0(elem->value);
                nlr_pop();
            } else {
                log_exc(MP_OBJ_FROM_PTR(nlr.ret_val));
            }
        }
    }
    g_res[index].alive = 0;
    drop_res_slots(index);
    MP_STATE_VM(nodemp_resource_globals)[index] = MP_OBJ_NULL;
    g_res[index].used = 0;
    g_current = prev;
}
