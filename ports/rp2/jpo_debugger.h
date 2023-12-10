#ifndef MICROPY_INCLUDED_RP2_JPO_DEBUGGER_H
#define MICROPY_INCLUDED_RP2_JPO_DEBUGGER_H

#include <stdbool.h>
#include "mpconfigport.h" // for JPO_DBGR_BUILD

#include "py/mpstate.h" // for jpo_bytecode_pos_t

// Minimal debugger features are always enabled
#define JPO_DBGR (1)

///////////////////////////
// Events/commands/requests
///////////////////////////
#if JPO_DBGR_BUILD
    // PC sends to start debugging.
    // Debugging will be stopped when the program terminates.
    #define CMD_DBG_START    "DBG_STRT"
    // Pause execution
    #define CMD_DBG_PAUSE    "DBG_PAUS"
    // Commands while paused
    #define CMD_DBG_CONTINUE "DBG_CONT"

    #define CMD_STEP_INTO  "DBG_SINT"
    #define CMD_STEP_OVER  "DBG_SOVR"
    #define CMD_STEP_OUT   "DBG_SOUT"

    // Events Brain sends when stopped
    #define EVT_DBG_STOPPED  "DBG_STOP" // + 8-byte reason str
    #define R_STOPPED_PAUSED     ":PAUSED_"    
    #define R_STOPPED_BREAKPOINT ":BREAKPT"
    #define R_STOPPED_STEP_INTO  ":SINT___"
    #define R_STOPPED_STEP_OVER  ":SOVR___"
    #define R_STOPPED_STEP_OUT   ":SOUT___"

    // Requests with responses
    #define REQ_DBG_STACK    "DBG_STAC"
#endif

// PC sends anytime to stop the program.
#define CMD_DBG_TERMINATE    "DBG_TRMT"
// Brain always sends when execution is done. 
#define EVT_DBG_DONE         "DBG_DONE" // + 4-byte int exit value


///////////////////
// Always available
///////////////////

/**
 * @brief Initialize the debugger. 
 * Call it even if not JPO_DBGR_BUILD, to support stopping the program etc.
 */
void jpo_dbgr_init(void);

/**
 * @brief Inform the PC that executing user code finished, either normally or with an error.
 * Call it even if not JPO_DBGR_BUILD.
 * Do not call on every module excution, only for the entire user program.
 * @param ret return value from parse_compile_execute
 */
void jpo_parse_compile_execute_done(int ret);

/**
 * Check and perform debugger actions as needed, before an opcode for given ip is executed.
 * Potentially blocks for a long time, returns when the user continues.
 * @param ip instruction pointer
 * @param bc_pos position within the bytecode, ip will be updated.
 *        Can't pass it as an arg, needs to be a variable in scope. 
 */
#if JPO_DBGR_BUILD
#define JPO_DBGR_PROCESS(ip) \
    if (dbgr_status != 0) { \
        if (bc_pos) { bc_pos->ip = ip; } \
        dbgr_process(bc_pos); \
    }
#else
#define JPO_DBGR_PROCESS(ip)
#endif //JPO_DBGR_BUILD


//////////////////////
// Debugger build only
//////////////////////
#if JPO_DBGR_BUILD

typedef enum _dbgr_status_t {
    // Debugging not enabled by the PC. Program might be running or done, irrelevant.
    DS_NOT_ENABLED = 0, // -> DS_RUNNING
    // Debugging enabled, program is running
    DS_RUNNING,     // -> DS_PAUSED
    // Pause was requested, with _stoppedReason to indicate why
    // Program will continue running in DS_STEP_IN mode until right before the next line
    DS_PAUSE_REQUESTED,

    // Stepping into/out/over code
    DS_STEP_INTO,
    DS_STEP_OUT,
    DS_STEP_OVER,

    // Stopped, waiting for commands (e.g. continue, breakpoints). _stoppedReason indicates why.
    // Fires a StoppedEvent when entering.
    DS_STOPPED,
    
    // Program terminated: DS_NOT_ENABLED
} dbgr_status_t;

/** @brief For internal use by JPO_DBGR_PROCESS. Do NOT set (except in debugger.c). */
extern dbgr_status_t dbgr_status;

/** @brief For internal use by JPO_DBGR_PROCESS. */
void dbgr_process(jpo_bytecode_pos_t *bc_pos);

/** @brief in vm.c, for use by debugger.c */
typedef struct _jpo_source_pos_t {
    qstr file;
    size_t line;
    qstr block;
    uint16_t depth;
} jpo_source_pos_t;
jpo_source_pos_t dbgr_get_source_pos(jpo_bytecode_pos_t *bc_pos);

/** @brief Diagonstics. Check if there is a stack overflow, DBG_SEND info. */
bool dbgr_check_stack_overflow(bool show_if_ok);
/** @brief Diagonstics. DBG_SEND stack info. */
void dbgr_print_stack_info(void);



// See RobotMesh

// // Initial check, send an event, see if the PC replies
// void dbgr_init(void);

// // Disable so it doesn't interfere with loading of built-ins
// void dbgr_disable(void);

// // True if debugger was disabled.
// // only used in dev.py
// uint8_t dbgr_isDisabled(void);

// /**
//  * Check for debug events at the start of every invocation.
//  * Returns PM_RET_OK if the execution should continue, or
//  * PM_RET_BREAK if it should break to prevent running of code
//  * (shouldRunIterate to be called on the next iteration).
//  */
// PmReturn_t dbgr_shouldRunIterate(void);

// /**
//  * Check if breaking is necessary. Returns PM_RET_OK if
//  * execution should continue, PM_RET_BREAK if it should break
//  * (a breakpoint is set or stepping through code).
//  */
// PmReturn_t dbgr_tryBreak(void);

// /**
//  * Break on error
//  */
// PmReturn_t dbgr_breakOnError(void);

#endif //JPO_DBGR_BUILD


#endif