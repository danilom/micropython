#include "jpo_dbgr_stackframes.h"

#include <stdio.h>
#include <string.h>

#include "jpo/jcomp_protocol.h"
#include "jpo/debug.h"
#include "jpo_debugger.h" // for dbgr_get_source_pos

#define CMD_LENGTH 8

// make it smaller for testing
//#define FRAME_PAYLOAD_SIZE JCOMP_MAX_PAYLOAD_SIZE
#define FRAME_PAYLOAD_SIZE 200

#define END_TOKEN "<end>"
#define END_TOKEN_SIZE (strlen(END_TOKEN) + 1)

static JCOMP_RV append_int_token(JCOMP_MSG msg, int num) {
    return jcomp_msg_append_uint32(msg, num);
}
static JCOMP_RV append_str_token(JCOMP_MSG msg, const char* str) {
    return jcomp_msg_append_bytes(msg, (uint8_t*) str, strlen(str) + 1);
}

static int get_frame_size(dbgr_bytecode_pos_t *bc_pos) {
    dbgr_source_pos_t source_pos = dbgr_get_source_pos(bc_pos);
    const char* file = qstr_str(source_pos.file);
    const char* block = qstr_str(source_pos.block);

    return (strlen(file) + 1) + (strlen(block) + 1) + 4 + 4;
}

/**
 * @brief Append a frame to a response
 * @returns JCOMP_OK if ok, an error (likely JCOMP_ERR_BUFFER_TOO_SMALL) if failed
 */
static JCOMP_RV append_frame(JCOMP_MSG resp, int frame_idx, dbgr_bytecode_pos_t *bc_pos) {
    JCOMP_RV rv = JCOMP_OK;

    dbgr_source_pos_t source_pos = dbgr_get_source_pos(bc_pos);
    const char* file = qstr_str(source_pos.file);
    const char* block = qstr_str(source_pos.block);

    // file
    rv = append_str_token(resp, file);
    if (rv) { return rv; }

    // block
    rv = append_str_token(resp, block);
    if (rv) { return rv; }

    // line
    rv = append_int_token(resp, source_pos.line);
    if (rv) { return rv; }

    // frame_idx
    rv = append_int_token(resp, frame_idx);
    if (rv) { return rv; }


    return JCOMP_OK;
}


/**
 * @brief send a reply to a stack request
 * @param request message, with a 4-byte start frame index
 * @return a packet of format <file>\0<block>\0<4b u32 line_num><4b u32 frame_idx>
 * "<end>" alone is a valid response.
 */
void dbgr_send_stack_response(const JCOMP_MSG request, dbgr_bytecode_pos_t *bc_stack_top) {
    if (bc_stack_top == NULL) {
        DBG_SEND("Error: send_stack_reply(): bc_stack_top is NULL");
        return;
    }
    
    // request: 8-byte name, 4-byte start frame index
    uint32_t start_frame_idx = jcomp_msg_get_uint32(request, CMD_LENGTH);
    DBG_SEND("stack request: start_frame_idx %d", start_frame_idx);
    
    JCOMP_CREATE_RESPONSE(resp, jcomp_msg_id(request), FRAME_PAYLOAD_SIZE);
    if (resp == NULL) {
        DBG_SEND("Error in send_stack_reply(): JCOMP_CREATE_RESPONSE failed");
    }

    JCOMP_RV rv = JCOMP_OK;

    dbgr_bytecode_pos_t *bc_pos = bc_stack_top;
    int frame_idx = 0;
    bool is_end = false;
    int pos = 0;
    while(true) {
        if (frame_idx >= start_frame_idx) {
            int frame_size = get_frame_size(bc_pos);
            if (pos + frame_size > FRAME_PAYLOAD_SIZE) {
                break;
            }
            pos += frame_size;

            rv = append_frame(resp, frame_idx, bc_pos);
            if (rv) { 
                DBG_SEND("Error in dbgr_send_stack_response: append_frame rv:%d", rv);
                return; 
            }
        }
        frame_idx++;
        bc_pos = bc_pos->caller_pos;
        if (bc_pos == NULL) {
            is_end = true;
            break;
        }
    }

    // if rv is not OK, we ran out of space in the response, just send what we have
    DBG_SEND("Done appending frames, count:%d pos:%d rv:%d", frame_idx, pos, rv);

    if (is_end) {
        if (pos + END_TOKEN_SIZE > FRAME_PAYLOAD_SIZE) {
            // no room for the end token, send what we have
        } else {
            // Apend the end token
            append_str_token(resp, END_TOKEN);
            pos += END_TOKEN_SIZE;
        }
    }
    
    jcomp_msg_set_payload_size(resp, pos);

    DBG_SEND("about to send stack response pos:%d payload_size:%d", pos, jcomp_msg_payload_size(resp));
    rv = jcomp_send_msg(resp);
    if (rv) {
        DBG_SEND("Error: send_stack_reply() failed: %d", rv);
    }
    DBG_SEND("done sending stack response");

}
