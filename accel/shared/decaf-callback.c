/*
Copyright (C) <2012> <Syracuse System Security (Sycure) Lab>

DECAF is based on QEMU, a whole-system emulator. You can redistribute
and modify it under the terms of the GNU GPL, version 3 or later,
but it is made available WITHOUT ANY WARRANTY. See the top-level
README file for more details.

For more information about DECAF and other softwares, see our
web site at:
http://sycurelab.ecs.syr.edu/

If you have any questions about DECAF,please post it on
http://code.google.com/p/decaf-platform/
*/
/*
 * DECAF_callback.c
 *
 *  Created on: Apr 10, 2012
 *      Author: heyin@syr.edu
 *  changed on: Nov 18, 2022
 *      Author: Aspen
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "shared/decaf-types-common.h"
#include "exec/exec-all.h"
#include "exec/cpu-defs.h"
#include "exec/helper-proto.h"

#include "shared/decaf-hashtable-wrapper.h"
#include "shared/decaf-callback-common.h"
#include "shared/decaf-callback-to-qemu.h"

#include "shared/decaf-main.h"

#include <sys/queue.h>

#if !defined(CONFIG_USER_ONLY)

#if defined(TARGET_I386) && (HOST_LONG_BITS == 64)
//#warning 64-bit macro
#define PUSH_ALL() __asm__ __volatile__ ("push %rax\npush %rcx\npush %rdx");
#define POP_ALL() __asm__ __volatile__ ("pop %rdx\npop %rcx\npop %rax");
#elif defined(TARGET_I386) && (HOST_LONG_BITS == 32)
//#warning 32-bit macro
#define PUSH_ALL() __asm__ __volatile__ ("push %eax\npush %ecx\npush %edx");
#define POP_ALL() __asm__ __volatile__ ("pop %edx\npop %ecx\npop %eax");
#else
//#warning Dummy PUSH_ALL() and POP_ALL() macros
#define PUSH_ALL() ;
#define POP_ALL() ;
#endif // AWH

// LOK: The callback logic is separated into two parts
//  1. the interface between QEMU and callback
//    this is invoked during translation time
//  2. the interface between the callback and the plugins
//    this is invoked during execution time
// The basic idea is to provide a rich interface for users
//  while abstracting and hiding all of the details

// Currently we support callbacks for instruction begin/end and
// basic block begin and end. Basic block level callbacks can
// also be optimized. There are two types of optimized callbacks
// constant (OCB_CONST) and page (OCB_PAGE). The idea begin
// the constant optimized callback type is that
// function level hooking only needs to know when the function
// starts and returns, both of which are hard target addresses.
// The idea behind the page level callback is for API level
// instrumentation where modules are normally separated at the
// page level. Block end callbacks are a little bit special
// since the callback can be set for both the 'from' and 'to'
// addresses. In this way, you can specify callbacks only for
// the transitions between a target module and the libc library
// for example. For simplicity sake we only provide page-level optimized
// callback support for block ends. Supporting it at the individual
// address level seemed like overkill - n^2 possible combinations.
// Todo: Future implementations should include support for the NOT
// condition for the optimized callbacks. This capability can be used
// for specifying all transitions from outside of a module into a module.
// for example it would be specified as block end callbacks where from
// is NOT module_page and to is module_page.


//We begin by declaring the necessary data structures
// for determining whether a callback is necessary
//These are declared if all block begins and ends
// are needed.
static int bEnableAllBlockBeginCallbacks = 0;
//We keep a count of the number of registered callbacks that
// need all of the block begins and ends so that we can turn
// on or off the optimized callback interface. The complete
// translation cache is flushed when the count goes from 0 to 1
// and from 1 downto 0
static int enableAllBlockBeginCallbacksCount = 0;
static int bEnableAllBlockEndCallbacks = 0;
static int enableAllBlockEndCallbacksCount = 0;


//We use hashtables to keep track of individual basic blocks
// that is associated with a callback - we ignore
// the conditional that is registered with the callback
// right now - that is the conditional has been changed
// into a simple "enable" bit. The reasoning is that the condition is controlled
// by the user, and so there is no way for us to update
// the hashtables accordingly.
//Specifically, we use a counting hashtable (essentially a hashmap)
// where the value is the number of callbacks that have registered for
// this particular condition. The affected translation block
// is flushed at the 0 to 1 or 1 to 0 transitions.
static counting_hashtable* pOBBTable;	//optimized block begin table

//A second page table at the page level
//There are two different hashtables so that determining
// whether a callback is needed at translation time (stage 1)
// can be done in the order of - allBlockBegins, Page level and then
// individual address or constant level.
static counting_hashtable* pOBBPageTable;

//Similarly we declare hashtables for block end
// the unique aspect of these hashtables is that they are
// only defined at the page level AND they are defined
// for the block end source (i.e. the location of the current
// basic block), the block end target (i.e. where is the next
// basic block), and then a map that contains both.

//This first table is for block ends callbacks that only specify the
// from address - i.e. where the block currently is
static counting_hashtable* pOBEFromPageTable;

//This second table is for callbacks that only specify the to address
// that is where the next block is
static counting_hashtable* pOBEToPageTable;

//This third table is for callbacks that specify both the from and to
// addresses. The hashmap maps the "from" page to a hashtable of "to" pages
static counting_hashmap* pOBEPageMap;


//data structures for storing the userspace callbacks (stage 2)
typedef struct callback_struct{
	int *enabled;
	//the following are used by the optimized callbacks
	//BlockBegin only uses from - to is ignored
	//blockend uses both from and to
	gva_t from;
	gva_t to;
	OCB_t ocb_type;

	DECAF_callback_func_t callback;
	QLIST_ENTRY(callback_struct) link;
} callback_struct;

// Each type of callback has its own callback_list
// The callback list is used to maintain the list of registered callbacks
// as well as their conditions. In essense, this data structure
// is used for interfacing with the user (stage 2)
static QLIST_HEAD(callback_list_head, callback_struct) callback_list_heads[DECAF_LAST_CB];

//Aravind - Serialized callbacks. 000 to 1ff, 1xx == 0fxx (for two byte opcodes)
static callback_struct* instructionCallbacks[0x200] = {0};

/***********************************************************************************************/
//Aravind - Function to register cb handlers for instruction ranges
DECAF_handle decaf_register_opcode_range_callback (
		DECAF_callback_func_t handler,
		OpcodeRangeCallbackConditions *condition,
		uint16_t start_opcode,
		uint16_t end_opcode)
{
	int i;

	if(end_opcode < start_opcode) {
		fprintf(stderr, "end_opcode can't be less than start_opcode.\n");
		return DECAF_NULL_HANDLE;
	}

	callback_struct * cb_struct = (callback_struct *)g_malloc(sizeof(callback_struct));
	if (cb_struct == NULL)
	{
	  return (DECAF_NULL_HANDLE);
	}

	if(start_opcode >= 0x0f00) {
		start_opcode = 0x100 | (start_opcode & 0xff);
	}

	if(end_opcode >= 0x0f00) {
		end_opcode = 0x100 | (end_opcode & 0xff);
	}

	cb_struct->callback = handler;
	cb_struct->from = start_opcode;
	cb_struct->to = end_opcode;
	cb_struct->enabled = (int *)condition;

	for(i = start_opcode; i <= end_opcode; i++) {
		instructionCallbacks[i] = cb_struct;
	}

	QLIST_INSERT_HEAD(&callback_list_heads[DECAF_OPCODE_RANGE_CB], cb_struct, link);

	//Flush the tb
  	register_decaf_flush_translation_cache(ALL_CACHE, 0);

	return (DECAF_handle)cb_struct;
}

//Function to unregister opcode range callbacks
DECAF_errno_t decaf_unregister_opcode_range_callback(DECAF_handle handle)
{
	callback_struct *cb_struct, *cb_next;
	int i;

	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_OPCODE_RANGE_CB], link, cb_next) {
		if((DECAF_handle)cb_struct != handle)
			continue;

		//Sanity check
		if(cb_struct->from > 0x1ff || cb_struct->to > 0x1ff || cb_struct->from > cb_struct->to)
			goto invalid_handle;

		for(i = cb_struct->from; i <= cb_struct->to; i++) {
			instructionCallbacks[i] = NULL;
		}

		QLIST_REMOVE(cb_struct, link);

		g_free(cb_struct);

		return 0;
	}

invalid_handle:
	return -1;
}

DECAF_handle decaf_register_optimized_block_begin_callback(
    DECAF_callback_func_t cb_func,
    int *cb_cond,
    gva_t addr,
    OCB_t type)
{
    callback_struct * cb_struct = (callback_struct *)g_malloc(sizeof(callback_struct));
    if (cb_struct == NULL) {
        return (DECAF_NULL_HANDLE);
    }

    //Heng: Optimization on OCB_CONST is not stable. We use OCB_ALL instead for now.
    if (type == OCB_CONST) type = OCB_ALL;

    //pre-populate the info
    cb_struct->callback = cb_func;
    cb_struct->enabled = cb_cond;
    cb_struct->from = addr;
    cb_struct->to = INV_ADDR;
    cb_struct->ocb_type = type;

    switch (type)
    {
        default:
        case (OCB_ALL):
        {
            //call the original
            bEnableAllBlockBeginCallbacks = 1;
            enableAllBlockBeginCallbacksCount++;

            //we need to flush if it just transitioned from 0 to 1
            if (enableAllBlockBeginCallbacksCount == 1)
            {
                // Perhaps we should flush ALL blocks instead of
                // just the ones associated with this cs?
                // tlb_flush() does that exactly
                register_decaf_flush_translation_cache(ALL_CACHE, 0);
            }
            break;
        }
        // case (OCB_CONST):
        // {
        //     if (pOBBTable == NULL)
        //     {
        //         g_free(cb_struct);
        //         return (DECAF_NULL_HANDLE);
        //     }
        //     //This is not necessarily thread-safe
        //     if (counting_hashtable_add(pOBBTable, addr) == 1)
        //     {
        //         register_decaf_flush_translation_cache(BLOCK_LEVEL, addr);
        //     }
        //     break;
        // }
        case (OCB_CONST_NOT):
        {
            break;
        }
        case (OCB_PAGE_NOT):
        {
            break;
        }
        case (OCB_PAGE):
        {
            addr &= TARGET_PAGE_MASK;
            if (pOBBPageTable == NULL) {
                g_free(cb_struct);
                return (DECAF_NULL_HANDLE);
            }

            //This is not necessarily thread-safe
            if (counting_hashtable_add(pOBBPageTable, addr) == 1) {
                register_decaf_flush_translation_cache(PAGE_LEVEL, addr);
            }
            break;
        }
    }

    //insert it into the list
    QLIST_INSERT_HEAD(&callback_list_heads[DECAF_BLOCK_BEGIN_CB], cb_struct, link);
    return ((DECAF_handle)cb_struct);
}

DECAF_errno_t decaf_unregister_optimized_block_begin_callback(DECAF_handle handle)
{
	callback_struct *cb_struct, *cb_next;

	// to unregister the callback, we have to first find the
	// callback and its conditions and then remove it from the
	// corresonding hashtable

	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_BLOCK_BEGIN_CB], link, cb_next) {
		if((DECAF_handle)cb_struct != handle)
			continue;

		//now that we have found it - check out its conditions
		switch(cb_struct->ocb_type)
		{
			default: //same as ALL to match the register function
			case (OCB_ALL):
            {
                enableAllBlockBeginCallbacksCount--;
                if (enableAllBlockBeginCallbacksCount == 0)
                {
                    bEnableAllBlockBeginCallbacks = 0;
                    //if its now zero flush the cache
                    register_decaf_flush_translation_cache(ALL_CACHE, 0);
                }
                else if (enableAllBlockBeginCallbacksCount < 0)
                {
                    // if it underflowed then reset to 0
                    // this is really an error
                    // notice I don't reset enableallblockbegincallbacks to 0
                    // just in case
                    enableAllBlockBeginCallbacksCount = 0;
                }
                break;
            }
			// case (OCB_CONST):
            // {
            //     if (pOBBTable == NULL)
            //     {
            //         return (NULL_POINTER_ERROR);
            //     }
            //     if (counting_hashtable_remove(pOBBTable, cb_struct->from) == 0)
            //     {
            //         register_decaf_flush_translation_cache(BLOCK_LEVEL, cb_struct->from);
            //     }
            //     break;
            // }
			case (OCB_PAGE):
            {
                if (pOBBPageTable == NULL)
                {
                    return (NULL_POINTER_ERROR);
                }
                if (counting_hashtable_remove(pOBBPageTable, cb_struct->from) == 0)
                {
                    register_decaf_flush_translation_cache(PAGE_LEVEL, cb_struct->from);
                }
                break;
            }
		}

		//now that we cleaned up the hashtables - we should remove the callback entry
		QLIST_REMOVE(cb_struct, link);
		//and free the struct
		g_free(cb_struct);

		return 0;
	}
	return -1;
}

DECAF_handle decaf_register_optimized_block_end_callback(
		DECAF_callback_func_t cb_func,
		int *cb_cond,
		gva_t from,
		gva_t to)
{
	callback_struct * cb_struct = (callback_struct *)g_malloc(sizeof(callback_struct));
	if (cb_struct == NULL)
	{
		return (DECAF_NULL_HANDLE);
	}

	//pre-populate the info
	cb_struct->callback = cb_func;
	cb_struct->enabled = cb_cond;
	cb_struct->from = from;
	cb_struct->to = to;
	cb_struct->ocb_type = OCB_ALL;

	if ( (from == INV_ADDR) && (to == INV_ADDR) )
	{
		enableAllBlockEndCallbacksCount++;
		bEnableAllBlockEndCallbacks = 1;
		if (enableAllBlockEndCallbacksCount == 1)
		{
			register_decaf_flush_translation_cache(ALL_CACHE, 0);
		}
	}
	else if (to == INV_ADDR) //this means only looking at the FROM list
	{
		if (pOBEFromPageTable == NULL)
		{
			g_free(cb_struct);
			return(DECAF_NULL_HANDLE);
		}

		if (counting_hashtable_add(pOBEFromPageTable, from & TARGET_PAGE_MASK) == 1)
		{
			register_decaf_flush_translation_cache(PAGE_LEVEL, from);
		}
	}
	else if (from == INV_ADDR)  //this is tricky, because it involves flushing the WHOLE cache	
	{
		if (pOBEToPageTable == NULL)
		{
			g_free(cb_struct);
			return(DECAF_NULL_HANDLE);
		}

		if (counting_hashtable_add(pOBEToPageTable, to & TARGET_PAGE_MASK) == 1)
		{
			register_decaf_flush_translation_cache(ALL_CACHE, 0);
		}
	}
	else
	{
		if (pOBEPageMap == NULL)
		{
			g_free(cb_struct);
			return(DECAF_NULL_HANDLE);
		}

		//if we are here then that means we need the hashmap
		if (counting_hashmap_add(pOBEPageMap, from & TARGET_PAGE_MASK, to & TARGET_PAGE_MASK) == 1)
		{
			register_decaf_flush_translation_cache(PAGE_LEVEL, from);
		}
	}

	//insert into the list
	QLIST_INSERT_HEAD(&callback_list_heads[DECAF_BLOCK_END_CB], cb_struct, link);
	return ((DECAF_handle)cb_struct);
}

int decaf_unregister_optimized_block_end_callback(DECAF_handle handle)
{
    callback_struct *cb_struct, *cb_next;

    // to unregister the callback, we have to first find the
    // callback and its conditions and then remove it from the
    // corresonding hashtable

    QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_BLOCK_END_CB], link, cb_next) {
        if((DECAF_handle)cb_struct != handle)
        continue;

        if ( (cb_struct->from == INV_ADDR) && (cb_struct->to == INV_ADDR) )
        {
            enableAllBlockEndCallbacksCount--;
            if (enableAllBlockEndCallbacksCount == 0)
            {
                register_decaf_flush_translation_cache(ALL_CACHE, 0);
                bEnableAllBlockEndCallbacks = 0;
            }
            else if (enableAllBlockEndCallbacksCount < 0)
            {
                //this is really an error
                enableAllBlockEndCallbacksCount = 0;
            }
        }
        else if (cb_struct->to == INV_ADDR)
        {
            gva_t from = cb_struct->from & TARGET_PAGE_MASK;
            if (counting_hashtable_remove(pOBEFromPageTable, from) == 0)
            {
                register_decaf_flush_translation_cache(PAGE_LEVEL, from);
            }
        }
        else if (cb_struct->from == INV_ADDR)
        {
            gva_t to = cb_struct->to & TARGET_PAGE_MASK;
            if (counting_hashtable_remove(pOBEToPageTable, to) == 0)
            {
                register_decaf_flush_translation_cache(ALL_CACHE, 0);
            }
        }
        else if (counting_hashmap_remove(pOBEPageMap, cb_struct->from & TARGET_PAGE_MASK, cb_struct->to & TARGET_PAGE_MASK) == 0)
        {
            register_decaf_flush_translation_cache(PAGE_LEVEL, cb_struct->from & TARGET_PAGE_MASK);
        }

        //we can now remove the entry
        QLIST_REMOVE(cb_struct, link);
        //and free the struct
        g_free(cb_struct);
        return 0;
    }
    return (-1);
}

DECAF_handle decaf_register_match_block_end_callback( //for the nbench plugin
    DECAF_callback_func_t cb_func,
    int *cb_cond,
    gva_t from,
    gva_t to)
{

    callback_struct * cb_struct = (callback_struct *)g_malloc(sizeof(callback_struct));
    if (cb_struct == NULL)
    {
        return (DECAF_NULL_HANDLE);
    }

    //pre-populate the info
    cb_struct->callback = cb_func;
    cb_struct->enabled = cb_cond;
    cb_struct->from = from;
    cb_struct->to = to;
    cb_struct->ocb_type = OCB_ALL;

    if ( (from == INV_ADDR) && (to == INV_ADDR) )
    {
        return DECAF_NULL_HANDLE;
    }
    else if (to == INV_ADDR) //this means only looking at the FROM list
    {
        if (pOBEFromPageTable == NULL)
        {
            g_free(cb_struct);
            return(DECAF_NULL_HANDLE);
        }

        if (counting_hashtable_add(pOBEFromPageTable, from & TARGET_PAGE_MASK) == 1)
        {
            register_decaf_flush_translation_cache(PAGE_LEVEL, from);
        }
    }
    else if (from == INV_ADDR) //this is tricky, because it involves flushing the WHOLE cache
    {
        if (pOBEToPageTable == NULL)
        {
            g_free(cb_struct);
            return(DECAF_NULL_HANDLE);
        }

        if (counting_hashtable_add(pOBEToPageTable, to & TARGET_PAGE_MASK) == 1)
        {
            register_decaf_flush_translation_cache(ALL_CACHE, 0);
        }
    }
    else
    {
        if (pOBEPageMap == NULL)
        {
            g_free(cb_struct);
            return(DECAF_NULL_HANDLE);
        }

        //if we are here then that means we need the hashmap
        if (counting_hashmap_add(pOBEPageMap, from & TARGET_PAGE_MASK, to & TARGET_PAGE_MASK) == 1)
        {
            register_decaf_flush_translation_cache(PAGE_LEVEL, from);
        }
    }

    //insert into the list
    QLIST_INSERT_HEAD(&callback_list_heads[DECAF_BLOCK_END_CB], cb_struct, link);
    return ((DECAF_handle)cb_struct);
}

// this is for backwards compatibility -
// for block begin and end - we make a call to the optimized versions
// for insn begin and end we just use the old logic
// for mem read and write ,use the old logic
DECAF_handle decaf_register_callback(
    DECAF_callback_type_t cb_type,
    DECAF_callback_func_t cb_func,
    int *cb_cond)
{
    if (cb_type == DECAF_BLOCK_BEGIN_CB)
    {
        return(decaf_register_optimized_block_begin_callback(cb_func, cb_cond, INV_ADDR, OCB_ALL));
    }

    if (cb_type == DECAF_BLOCK_END_CB)
    {
        return(decaf_register_optimized_block_end_callback(cb_func, cb_cond, INV_ADDR, INV_ADDR));
    }

    //if we are here then that means its either insn begin or end - this is the old logic no changes

    callback_struct * cb_struct = (callback_struct *)g_malloc(sizeof(callback_struct));

    if(cb_struct == NULL)
        return (DECAF_NULL_HANDLE);

    cb_struct->callback = cb_func;
    cb_struct->enabled = cb_cond;


    if(cb_type == DECAF_TLB_EXEC_CB)
	    goto insert_callback; //Should not flush for tlb callbacks since they don't go into tb.

// AVB ,Do we need a flush here?
    if(QLIST_EMPTY(&callback_list_heads[cb_type]))
        register_decaf_flush_translation_cache(ALL_CACHE, 0);

insert_callback:
    QLIST_INSERT_HEAD(&callback_list_heads[cb_type], cb_struct, link);
    return (DECAF_handle)cb_struct;
}

int decaf_unregister_callback(DECAF_callback_type_t cb_type, DECAF_handle handle)
{
    if (cb_type == DECAF_BLOCK_BEGIN_CB)
    {
        return (decaf_unregister_optimized_block_begin_callback(handle));
    }
    else if (cb_type == DECAF_BLOCK_END_CB)
    {
        return (decaf_unregister_optimized_block_end_callback(handle));
    }

    callback_struct *cb_struct, *cb_next;
    //FIXME: not thread safe
    QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[cb_type], link, cb_next) {
        if((DECAF_handle)cb_struct != handle)
            continue;

        QLIST_REMOVE(cb_struct, link);
        g_free(cb_struct);

        if(cb_type == DECAF_TLB_EXEC_CB) {
            goto done;
        }

        if(QLIST_EMPTY(&callback_list_heads[cb_type])) {
            register_decaf_flush_translation_cache(ALL_CACHE, 0);
        }
done:
        return 0;
    }
    return -1;
}

void decaf_callback_init(void)
{
    int i;

    for(i=0; i<DECAF_LAST_CB; i++)
        QLIST_INIT(&callback_list_heads[i]);

    pOBBTable = counting_hashtable_new();
    pOBBPageTable = counting_hashtable_new();

    pOBEFromPageTable = counting_hashtable_new();
    pOBEToPageTable = counting_hashtable_new();
    pOBEPageMap = counting_hashmap_new();

    bEnableAllBlockBeginCallbacks = 0;
    enableAllBlockBeginCallbacksCount = 0;
    bEnableAllBlockEndCallbacks = 0;
    enableAllBlockEndCallbacksCount = 0;
}
/***********************************************************************************************/

// LOK: I turned this into a dumb function
// and so we can have the more specialized helper functions
// for the block begin and block end callbacks
int decaf_is_callback_needed(DECAF_callback_type_t cb_type)
{
    return !QLIST_EMPTY(&callback_list_heads[cb_type]);
}

int decaf_is_callback_needed_for_opcode(int op)
{
	if(op < 0x200 && instructionCallbacks[op] != NULL)
		return 1;

	return 0;
}

//here we search from the broadest to the narrowest
// to determine whether the callback is needed
int decaf_is_block_begin_callback_needed(gva_t pc)
{
    //go through the page list first
    if (bEnableAllBlockBeginCallbacks)
    {
        return (1);
    }

    if (counting_hashtable_exist(pOBBPageTable, pc & TARGET_PAGE_MASK))
    {
        return (1);
    }

    if (counting_hashtable_exist(pOBBTable, pc))
    {
        return (1);
    }

    return 0;
}

int decaf_is_block_end_callback_needed(gva_t from, gva_t to)
{
    if (bEnableAllBlockEndCallbacks)
    {
        return (1);
    }

    from &= TARGET_PAGE_MASK;
    //go through the page list first
    if (counting_hashtable_exist(pOBEFromPageTable, from))
    {
        return (1);
    }
    if (to == INV_ADDR) //this is a special case where the target is not known at translation time
    {
        return (0);
    }

    to &= TARGET_PAGE_MASK;
    if (counting_hashtable_exist(pOBEToPageTable, to))
    {
        return (1);
    }

    return (counting_hashmap_exist(pOBEPageMap, from, to));
}
/***********************************************************************************************/

void helper_decaf_invoke_block_begin_callback(CPUState* cs, TranslationBlock* tb)
{
    static callback_struct *cb_struct, *cb_next;
    static DECAF_Callback_Params params;

    if ((cs == NULL) || (tb == NULL))
    {
        return;
    }

    params.bb.cs = cs;
    params.bb.tb = tb;

    PUSH_ALL()

    //FIXME: not thread safe
    QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_BLOCK_BEGIN_CB], link, cb_next){
        // If it is a global callback or it is within the execution context,
        // invoke this callback
        if(!cb_struct->enabled || *cb_struct->enabled){
            params.cbhandle = (DECAF_handle)cb_struct;
            switch (cb_struct->ocb_type)
            {
                default:
                case (OCB_ALL):
                {
                    cb_struct->callback(&params);
                    break;
                }
                // case (OCB_CONST):
                // {
                //     if (cb_struct->from == tb->pc)
                //     {
                //         cb_struct->callback(&params);
                //     }
                //     break;
                // }
                case (OCB_PAGE):
                {
                    if ((cb_struct->from & TARGET_PAGE_MASK) == (tb->pc & TARGET_PAGE_MASK))
                    {
                        cb_struct->callback(&params);
                    }
                    break;
                }
            }
        }
    }
    POP_ALL()
}

void helper_decaf_invoke_block_end_callback(CPUState *cs, TranslationBlock *tb, gva_t from)
{
    static callback_struct *cb_struct, *cb_next;
    static DECAF_Callback_Params params;

    if (cs == NULL) return;

    params.be.cs = cs;
    params.be.tb = tb;
    params.be.cur_pc = from;

    PUSH_ALL()

#ifdef TARGET_I386
    X86CPU *cpu = X86_CPU(cs);
    CPUX86State *env = &cpu->env;
    params.be.next_pc = env->eip + env->segs[R_CS].base;
#elif defined(TARGET_ARM)
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    params.be.next_pc = env->regs[15];
#elif defined(TARGET_MIPS)
    MIPSCPU *cpu = MIPS_CPU(cs);
    CPUMIPSState *env = &cpu->env;
    params.be.next_pc = env->active_tc.PC;
#endif

    //FIXME: not thread safe
    QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_BLOCK_END_CB], link, cb_next) {
        // If it is a global callback or it is within the execution context,
        // invoke this callback
        if(!cb_struct->enabled || *cb_struct->enabled)
        {
            params.cbhandle = (DECAF_handle)cb_struct;
            if (cb_struct->to == INV_ADDR)
            {
                cb_struct->callback(&params);
            }
            else if ( (cb_struct->to & TARGET_PAGE_MASK) == (params.be.next_pc & TARGET_PAGE_MASK) )
            {
                if (cb_struct->from == INV_ADDR)
                {
                    cb_struct->callback(&params);
                }
                else if ( (cb_struct->from & TARGET_PAGE_MASK) == (params.be.cur_pc & TARGET_PAGE_MASK) )
                {
                    cb_struct->callback(&params);
                }
            }
        }
    }
    POP_ALL()
}

void helper_decaf_invoke_insn_begin_callback(CPUState *cs)
{
	static callback_struct *cb_struct, *cb_next;
	static DECAF_Callback_Params params;

	if (cs == 0) return;

	params.ib.cs = cs;
    PUSH_ALL()
	//FIXME: not thread safe
	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_INSN_BEGIN_CB], link, cb_next) {
		// If it is a global callback or it is within the execution context,
		// invoke this callback
        params.cbhandle = (DECAF_handle)cb_struct;
		if(!cb_struct->enabled || *cb_struct->enabled)
			cb_struct->callback(&params);
	}
    POP_ALL()
}

void helper_decaf_invoke_insn_end_callback(CPUState *cs)
{
	static callback_struct *cb_struct, *cb_next;
    static DECAF_Callback_Params params;

	if (cs == 0) return;
	params.ie.cs = cs;
    PUSH_ALL()
	//FIXME: not thread safe
	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_INSN_END_CB], link,cb_next) {
		// If it is a global callback or it is within the execution context,
		// invoke this callback
	    params.cbhandle = (DECAF_handle)cb_struct;
		if(!cb_struct->enabled || *cb_struct->enabled)
			cb_struct->callback(&params);
	}
    POP_ALL()
}

void helper_decaf_invoke_eip_check_callback(gva_t source_eip, gva_t target_eip, gva_t target_eip_taint)
{
	static callback_struct *cb_struct, *cb_next;
	static DECAF_Callback_Params params;

	params.ec.source_eip = source_eip;
    params.ec.target_eip = target_eip;
    params.ec.target_eip_taint = target_eip_taint;
    PUSH_ALL()
	//FIXME: not thread safe
	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_EIP_CHECK_CB], link, cb_next)
	{
		// If it is a global callback or it is within the execution context,
		// invoke this callback
        params.cbhandle = (DECAF_handle)cb_struct;
		if (!cb_struct->enabled || *cb_struct->enabled) {
			cb_struct->callback(&params);
		}
	}
	POP_ALL() // AWH
}

void helper_decaf_invoke_opcode_range_callback(
		CPUState *cs,
		decaf_target_ulong eip,
		decaf_target_ulong next_eip,
		uint32_t op)
{
	callback_struct *cb_struct = instructionCallbacks[op];
	if(cb_struct == NULL || cs == NULL)
		return;

	//OpcodeRangeCallbackConditions *cond = (OpcodeRangeCallbackConditions *)(cb_struct->enabled);
	OpcodeRangeCallbackConditions temp;

	//FIXME: Being naive and assuming that kernel starts from 0x80000000.
	//Correct way to do this would be to expose an interface from vmi to indicate the kernel base.
	uint32_t kernel_base = 0x80000000;
	int from_user, from_kernel, to_user, to_kernel;
	from_user = from_kernel = to_user = to_kernel = 0;

	if(*(cb_struct->enabled) != DECAF_ALL) {
		if(eip > kernel_base) {
			from_kernel = 1;
		} else {
			from_user = 1;
		}

		if(next_eip > kernel_base) {
			to_kernel = 1;
		} else {
			to_user = 1;
		}

		if(from_user & to_user) {
			temp = DECAF_USER_TO_USER_ONLY;
		} else if(from_user & to_kernel) {
			temp = DECAF_USER_TO_KERNEL_ONLY;
		} else if(from_kernel & to_user) {
			temp = DECAF_KERNEL_TO_USER_ONLY;
		} else {
			temp = DECAF_KERNEL_TO_KERNEL_ONLY;
		}

		//Condition violated
		if(temp & (*(cb_struct->enabled) == 0) )
			return;
	}

	DECAF_Callback_Params params;
	params.op.cs = cs;
	params.op.eip = eip;
	params.op.next_eip = next_eip;
	params.op.op = op;

	cb_struct->callback(&params);
}


void decaf_invoke_tlb_exec_callback(CPUState *cs, gva_t vaddr)
{
    callback_struct *cb_struct, *cb_next;
    DECAF_Callback_Params params;

    if ((cs == NULL) || (vaddr == 0)) {
        return;
    }
    params.tx.cs = cs;
    params.tx.vaddr = vaddr;
    QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_TLB_EXEC_CB], link, cb_next) {
        if(!cb_struct->enabled || *cb_struct->enabled) {
            cb_struct->callback(&params);
        }
    }
}


void helper_decaf_invoke_nic_rec_callback(uint8_t * buf, int size, int cur_pos, int start, int stop)
{
	static callback_struct *cb_struct, *cb_next;
	static DECAF_Callback_Params params;
	params.nr.buf = buf;
	params.nr.size = size;
	params.nr.cur_pos = cur_pos;
	params.nr.start = start;
	params.nr.stop = stop;
    PUSH_ALL()
	//FIXME: not thread safe
	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_NIC_REC_CB], link, cb_next)
	{
		// If it is a global callback or it is within the execution context,
		// invoke this callback
        params.cbhandle = (DECAF_handle)cb_struct;
		if (!cb_struct->enabled || *cb_struct->enabled) {
            cb_struct->callback(&params);
        }	
	}
    POP_ALL()
}

void helper_decaf_invoke_nic_send_callback(uint32_t addr, int size, uint8_t *buf)
{
	static callback_struct *cb_struct, *cb_next;
	static DECAF_Callback_Params params;
	params.ns.addr = addr;
	params.ns.size = size;
	params.ns.buf = buf;
    PUSH_ALL() // AWH
	//FIXME: not thread safe
	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_NIC_SEND_CB], link, cb_next)
	{
		// If it is a global callback or it is within the execution context,
		// invoke this callback
        params.cbhandle = (DECAF_handle)cb_struct;
		if (!cb_struct->enabled || *cb_struct->enabled)
			cb_struct->callback(&params);
	}
    POP_ALL() // AWH
}

void helper_decaf_invoke_mem_read_callback(gva_t vaddr, ram_addr_t paddr, unsigned long value, DATA_TYPE data_type)
{
    static callback_struct *cb_struct, *cb_next;
    static DECAF_Callback_Params params;
    params.mr.dt = data_type;
    params.mr.paddr = paddr;
    params.mr.vaddr = vaddr;
    params.mr.value = value;
    //if (cpu_single_cs == 0) return;
    PUSH_ALL()
    //FIXME: not thread safe
    QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_MEM_READ_CB], link, cb_next)
    {
        // If it is a global callback or it is within the execution context,
        // invoke this callback
        params.cbhandle = (DECAF_handle)cb_struct;
        if (!cb_struct->enabled || *cb_struct->enabled) {
            cb_struct->callback(&params);
        }
    }
    POP_ALL()
}

void helper_decaf_invoke_mem_write_callback(gva_t vaddr, ram_addr_t paddr, unsigned long value, DATA_TYPE data_type)
{
	static callback_struct *cb_struct, *cb_next;
	static DECAF_Callback_Params params;
	params.mw.dt = data_type;
    params.mw.paddr = paddr;
	params.mw.vaddr = vaddr;
    params.mw.value = value;
    PUSH_ALL()
	//if (cpu_single_cs == 0) return;

	//FIXME: not thread safe
	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_MEM_WRITE_CB], link, cb_next)
	{
		// If it is a global callback or it is within the execution context,
		// invoke this callback
        params.cbhandle = (DECAF_handle)cb_struct;
		if (!cb_struct->enabled || *cb_struct->enabled)
			cb_struct->callback(&params);
	}
    POP_ALL()
}

void helper_decaf_invoke_keystroke_callback(int keycode, uint32_t *taint_mark)
{
	static callback_struct *cb_struct, *cb_next;
	static DECAF_Callback_Params params;
	params.ks.keycode=keycode;
	params.ks.taint_mark=taint_mark;
	//if (cpu_single_cs == 0) return;

	//FIXME: not thread safe
	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_KEYSTROKE_CB], link, cb_next)
	{
		// If it is a global callback or it is within the execution context,
		// invoke this callback
        params.cbhandle = (DECAF_handle)cb_struct;
		if (!cb_struct->enabled || *cb_struct->enabled) {
			cb_struct->callback(&params);
		}
	}
}

void helper_decaf_invoke_read_taint_mem(gva_t vaddr, ram_addr_t paddr, uint32_t size, uint8_t *taint_info)
{
	static callback_struct *cb_struct, *cb_next;
	static DECAF_Callback_Params params;
	params.rt.vaddr = vaddr;
    params.rt.paddr = paddr;
	params.rt.size = size;
	params.rt.taint_info = taint_info;
    PUSH_ALL()
	//FIXME: not thread safe
	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_READ_TAINTMEM_CB], link, cb_next)
	{
		// If it is a global callback or it is within the execution context,
		// invoke this callback
        params.cbhandle = (DECAF_handle)cb_struct;
		if (!cb_struct->enabled || *cb_struct->enabled)
			cb_struct->callback(&params);
	}
    POP_ALL()
}

void helper_decaf_invoke_write_taint_mem(gva_t vaddr, ram_addr_t paddr, uint32_t size, uint8_t *taint_info)
{
	static callback_struct *cb_struct, *cb_next;
	static DECAF_Callback_Params params;
	params.wt.paddr = paddr;
	params.wt.vaddr = vaddr;
	params.wt.size = size;
	params.wt.taint_info = taint_info;
    PUSH_ALL()
	//FIXME: not thread safe
	QLIST_FOREACH_SAFE(cb_struct, &callback_list_heads[DECAF_WRITE_TAINTMEM_CB], link, cb_next)
	{
		// If it is a global callback or it is within the execution context,
		// invoke this callback
        params.cbhandle = (DECAF_handle)cb_struct;
		if (!cb_struct->enabled || *cb_struct->enabled)
			cb_struct->callback(&params);
	}
    POP_ALL()
}

#endif