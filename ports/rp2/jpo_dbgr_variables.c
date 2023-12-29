#include "jpo_debugger.h" 
#include "jpo_dbgr_protocol.h"

#include <stdio.h>
#include <string.h>

#include "py/bc.h" // for mp_code_state_t
#include "py/objfun.h"
#include "py/obj.h"

#include "jpo/jcomp_protocol.h"
#include "jpo/debug.h"

// Disable debugging
// #undef DBG_SEND
// #define DBG_SEND(...)

#define VARS_PAYLOAD_SIZE JCOMP_MAX_PAYLOAD_SIZE
#define OBJ_RER_MAX_SIZE 50

void dbg_print_obj(int i, mp_obj_t obj) {
    if (obj) {
        mp_printf(&mp_plat_print, "[%d] t:%s ", i, mp_obj_get_type_str(obj));
        mp_obj_print(obj, PRINT_REPR);
        mp_printf(&mp_plat_print, "\n");
    }
    else {
        mp_printf(&mp_plat_print, "[%d] NULL\n", i);
    }
}

typedef struct {
    enum _var_scope_type_t scope_type;
    enum _varinfo_kind_t include_kind;
    int depth_or_addr;
    int var_start_idx;
} vars_request_t;

typedef struct {
    vstr_t name;
    vstr_t value;
    qstr type;
    uint32_t address;
} varinfo_t;

static void varinfo_clear(varinfo_t* vi) {
    vstr_clear(&(vi->name));
    vi->name.len = 0;
    
    vstr_clear(&(vi->value));
    vi->value.len = 0;

    vi->type = 0; // qstr
    vi->address = 0;
}

typedef struct {
    const vars_request_t* args;

    // Option 1: iterate a dict
    mp_obj_dict_t* dict;
    // If true, print key as REPR, otherwise as STR
    bool dict_key_use_repr;

    // Option 2: iterate a list
    int objs_size;
    const mp_obj_t* objs;
    bool obj_names_are_indexes;

    int cur_idx;
    varinfo_t vi;
} vars_iter_t;

static void iter_clear(vars_iter_t* iter) {
    iter->dict = NULL;
    iter->dict_key_use_repr = false;

    iter->objs_size = 0;
    iter->objs = NULL;
    iter->obj_names_are_indexes = false;

    iter->cur_idx = -1;
    varinfo_clear(&iter->vi);
}

static void varinfo_set_type(varinfo_t* varinfo, mp_obj_t obj) {
    // type: always set
    const mp_obj_type_t* obj_type = mp_obj_get_type(obj);
    varinfo->type = obj_type->name;
}
static void varinfo_set_address(varinfo_t* varinfo, mp_obj_t obj) {
    // address: set for certain types, so debugger can drill down to examine the object
    if (mp_obj_is_type(obj, &mp_type_object)
        || mp_obj_is_type(obj, &mp_type_tuple)
        || mp_obj_is_type(obj, &mp_type_list)
        || mp_obj_is_type(obj, &mp_type_dict)) 
    {
        varinfo->address = (uint32_t)obj;
    }
}
static void iter_init_from_obj(vars_iter_t* iter, mp_obj_t obj) {
    iter_clear(iter);

    if (mp_obj_is_type(obj, &mp_type_object)) {
        // Returns a list of attributes
        mp_obj_t attr_list = mp_builtin_dir(1, &obj);
        // TODO: set a flag on iter to get attribute values
        size_t len = 0;
        mp_obj_t* items = NULL;
        mp_obj_get_array(attr_list, &len, &items);

        iter->objs_size = len;
        iter->objs = items;
    }
    else if (mp_obj_is_type(obj, &mp_type_tuple)
            || mp_obj_is_type(obj, &mp_type_list)) 
    {
        // TODO: add length
        size_t len = 0;
        mp_obj_t* items = NULL;
        mp_obj_get_array(obj, &len, &items);

        iter->objs_size = len;
        iter->objs = items;
        iter->obj_names_are_indexes = true;
    }
    else if (mp_obj_is_type(obj, &mp_type_dict)) {
        // TODO: add length, maybe
        iter->dict = MP_OBJ_TO_PTR(obj);
        iter->dict_key_use_repr = true;
    }
    else {
        DBG_SEND("Error: iter_init_from_obj(): unknown type:%s", mp_obj_get_type_str(obj));
    }
}

static void iter_init(vars_iter_t* iter, const vars_request_t* args, mp_obj_frame_t* top_frame) {
    iter_clear(iter);

    if (args->scope_type == VSCOPE_FRAME) {
        mp_obj_frame_t* frame = dbgr_find_frame(args->depth_or_addr, top_frame);
        if (frame == NULL) {
            return;
        }
        const mp_code_state_t* cur_bc = frame->code_state;
        iter->objs_size = cur_bc->n_state;
        iter->objs = cur_bc->state;

        // Micropython issue: this returns the same items as globals
        // iter->dict = MP_STATE_THREAD(dict_locals);
    }
    else if (args->scope_type == VSCOPE_GLOBAL) {
        iter->dict = MP_STATE_THREAD(dict_globals);
    }
    else if (args->scope_type == VSCOPE_OBJECT) {
        // Get the object. Hope the address is ok
        if (args->depth_or_addr == 0) {
            DBG_SEND("Error: iter_start(): object address is 0");
            return;
        }
        mp_obj_t obj = (mp_obj_t)args->depth_or_addr;
        iter_init_from_obj(iter, obj);
    }
    else {
        DBG_SEND("Error: iter_start(): unknown scope_type:%d", args->scope_type);
        return;
    }
}

// helper
static void obj_to_vstr(mp_obj_t obj, vstr_t* out_vstr, mp_print_kind_t print_kind) {
    mp_print_t print_to_vstr;
    vstr_init_print(out_vstr, OBJ_RER_MAX_SIZE, &print_to_vstr);
    mp_obj_print_helper(&print_to_vstr, obj, print_kind);
}

static varinfo_t* iter_next_dict(vars_iter_t* iter) {
    if (iter->dict == NULL) {
        return NULL;
    }

    if (iter->cur_idx == -1) {
        iter->cur_idx = 0;
    }

    // Advances to the next item
    mp_map_elem_t* elem = dict_iter_next(iter->dict, (size_t*)&(iter->cur_idx));
    if (elem == NULL) {
        return NULL;
    }

    varinfo_t* vi = &(iter->vi); 

    // clear previous info
    varinfo_clear(vi);

    // name is key
    obj_to_vstr(elem->key, &(vi->name), iter->dict_key_use_repr ? PRINT_REPR : PRINT_STR);

    // value
    obj_to_vstr(elem->value, &(vi->value), PRINT_REPR);

    varinfo_set_type(vi, elem->value);
    varinfo_set_address(vi, elem->value);

    return vi;
}
static varinfo_t* iter_next_list(vars_iter_t* iter) {
    if (iter->objs == NULL) {
        return NULL;
    }

    // advance to the next
    iter->cur_idx++;
    if (iter->cur_idx >= iter->objs_size) {
        return NULL;
    }

    //DBG_SEND("iter_next() idx:%d size:%d", iter->cur_idx, iter->objs_size);

    // clear previous info
    varinfo_t* vi = &(iter->vi);
    varinfo_clear(vi);

    // might be null
    mp_obj_t obj = iter->objs[iter->cur_idx];
    //dbg_print_obj(iter->cur_idx, obj);

    if (obj != NULL) {
        // name
        // for local vars (VSCOPE_FRAME/VKIND_VARIABLES) names are not available
        if (iter->obj_names_are_indexes) {
            vstr_init(&vi->name, 6);
            vstr_printf(&vi->name, "%d", iter->cur_idx);
        }

        // value
        obj_to_vstr(obj, &(vi->value), PRINT_REPR);

        varinfo_set_type(vi, obj);
        varinfo_set_address(vi, obj);
    }

    //DBG_SEND("iter_next_list() done");

    return vi;
}

// NULL if no more items
static varinfo_t* iter_next(vars_iter_t* iter) {
    if (iter->dict != NULL) {
        return iter_next_dict(iter);
    }
    else if (iter->objs != NULL) {
        return iter_next_list(iter);
    }
    return NULL;
}

static int varinfo_get_size(varinfo_t* vi) {
    // name, value, type, address
    //DBG_SEND("length of name:%d value:%d type:%d", vi->name.len, vi->value.len, strlen(qstr_str(vi->type)));
    return (vi->name.len + 1 + 
            vi->value.len + 1 +
            strlen(qstr_str(vi->type)) + 1 + 
            4);
}
static void varinfo_append(varinfo_t* vi, JCOMP_MSG resp) {
    // Ignoring rv for now
    const char* name = vstr_str(&(vi->name));
    const char* value = vstr_str(&(vi->value));
    const char* type = qstr_str(vi->type); 

    if (name == NULL) { name = ""; }
    if (value == NULL) { value = ""; }
    if (type == NULL) { type = ""; }

    jcomp_msg_append_str0(resp, name);
    jcomp_msg_append_str0(resp, value);
    jcomp_msg_append_str0(resp, type);
    jcomp_msg_append_uint32(resp, vi->address);
}

varinfo_kind_t varinfo_get_kind(varinfo_t* vi) {
    // DBG_SEND("var %s:%s len:%d [0]:%d [1]:%d", 
    //     vstr_str(&vi->name), vstr_str(&vi->value), 
    //     vstr_len(&vi->name), vstr_str(&vi->name)[0], vstr_str(&vi->name)[1]);

    // starts with "__"
    if (vstr_len(&vi->name) >= 2 && vstr_str(&vi->name)[0] == '_' && vstr_str(&vi->name)[1] == '_') {
        return VKIND_SPECIAL;
    }
    else if (vi->type == mp_type_fun_bc.name) {
        return VKIND_FUNCTION;
    }
    else if (vi->type == mp_type_type.name) {
        return VKIND_CLASS;
    }
    else if (vi->type == mp_type_module.name) {
        return VKIND_MODULE;
    }
    else {
        return VKIND_NORMAL;
    }
}
void send_vars_response(uint8_t req_id, const vars_request_t* args, mp_obj_frame_t* top_frame) {
    DBG_SEND("send_vars_response: req: scope_type:%d include_kind:%d depth_or_addr:%d var_start_idx:%d",
        args->scope_type, args->include_kind, args->depth_or_addr, args->var_start_idx);

    JCOMP_CREATE_RESPONSE(resp, req_id, VARS_PAYLOAD_SIZE);
    if (resp == NULL) {
        DBG_SEND("Error in send_vars_response(): JCOMP_CREATE_RESPONSE failed");
    }

    int pos = 0;
    varinfo_kind_t contains_flags = 0;
    jcomp_msg_append_byte(resp, 0); // write again when we know the flags
    pos += 1;

    // Fill the items
    vars_iter_t iter;
    iter_init(&iter, args, top_frame);

    int var_idx = 0;
    bool packet_full = false;
    while(true) {
        varinfo_t* vi = iter_next(&iter);
        if (vi == NULL) {
            break;
        }

        // See if we want to include it
        varinfo_kind_t kind = varinfo_get_kind(vi);
        //DBG_SEND("var %s:%s (%s) kind: %d", vstr_str(&vi->name), vstr_str(&vi->value), qstr_str(vi->type),  kind);
        contains_flags |= kind;
        
        if (kind & args->include_kind) {
            //DBG_SEND("loop: iter->cur_idx:%d var_idx:%d var_start_idx:%d", iter.cur_idx, var_idx, args->var_start_idx);
            if (var_idx >= args->var_start_idx 
                && !packet_full) 
            {
                int append_size = varinfo_get_size(vi);
                if (pos + append_size >= VARS_PAYLOAD_SIZE) {
                    // no room for this item, send what we have
                    packet_full = true;
                }
                else {
                    varinfo_append(vi, resp);
                    pos += append_size;
                }
            }
            var_idx++;
        }
    }

    if (!packet_full) {        
        if (pos + END_TOKEN_SIZE >= VARS_PAYLOAD_SIZE) {
            // no room for the end token, send what we have
        } else {
            // Apend the end token
            jcomp_msg_append_str0(resp, END_TOKEN);
            pos += END_TOKEN_SIZE;
        }
    }

    iter_clear(&iter);

    // Set the subsection flags
    jcomp_msg_set_byte(resp, 0, contains_flags);

    jcomp_msg_set_payload_size(resp, pos);

    JCOMP_RV rv = jcomp_send_msg(resp);
    if (rv) {
        DBG_SEND("Error: send_vars_response() send failed: %d", rv);
    }

    DBG_SEND("send_vars_response(): done");
}

void dbgr_send_variables_response(const JCOMP_MSG request, mp_obj_frame_t* top_frame) {
    if (top_frame == NULL || request == NULL) {
        DBG_SEND("Error: dbgr_send_variables_response(): top_frame or request is NULL");
        return;
    }

    int pos = CMD_LENGTH;
    var_scope_type_t scope_type = (var_scope_type_t)jcomp_msg_get_byte(request, pos++);
    varinfo_kind_t include_kind = (varinfo_kind_t)jcomp_msg_get_byte(request, pos++);
    int depth_or_addr = jcomp_msg_get_uint32(request, pos);
    pos += 4;
    int var_start_idx = jcomp_msg_get_uint32(request, pos);
    pos += 4;

    vars_request_t args = {
        .scope_type = scope_type,
        .include_kind = include_kind,
        .depth_or_addr = depth_or_addr,
        .var_start_idx = var_start_idx,
    };
    send_vars_response(jcomp_msg_id(request), &args, top_frame);
}