/**
 @file stackinfo.cpp

 @brief Implements the stackinfo class.
 */

#include "stackinfo.h"
#include "memory.h"
#include "disasm_helper.h"
#include "disasm_fast.h"
#include "_exports.h"
#include "module.h"
#include "thread.h"
#include "threading.h"
#include "exhandlerinfo.h"
#include "symbolinfo.h"

using SehMap = std::unordered_map<duint, STACK_COMMENT>;
static SehMap SehCache;

void stackupdateseh()
{
    SehMap newcache;
    std::vector<duint> SEHList;
    if(ExHandlerGetSEH(SEHList))
    {
        STACK_COMMENT comment;
        strcpy_s(comment.color, "!sehclr"); // Special token for SEH chain color.
        auto count = SEHList.size();
        for(duint i = 0; i < count; i++)
        {
            if(i + 1 != count)
                sprintf_s(comment.comment, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "Pointer to SEH_Record[%d]")), i + 1);
            else
                sprintf_s(comment.comment, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "End of SEH Chain")));
            newcache.insert({ SEHList[i], comment });
        }
    }
    EXCLUSIVE_ACQUIRE(LockSehCache);
    SehCache = std::move(newcache);
}

static void getSymAddrName(duint addr, char* str)
{
    ADDRINFO addrinfo;
    if(addr == 0)
    {
        memcpy(str, "???", 4);
        return;
    }
    addrinfo.flags = flaglabel | flagmodule;
    _dbg_addrinfoget(addr, SEG_DEFAULT, &addrinfo);
    if(addrinfo.module[0] != '\0')
        sprintf_s(str, MAX_COMMENT_SIZE, "%s.", addrinfo.module);
    if(addrinfo.label[0] == '\0')
        sprintf_s(addrinfo.label, MAX_COMMENT_SIZE, "%p", addr);
    strcat_s(str, MAX_COMMENT_SIZE, addrinfo.label);
}

bool stackcommentget(duint addr, STACK_COMMENT* comment)
{
    SHARED_ACQUIRE(LockSehCache);
    const auto found = SehCache.find(addr);
    if(found != SehCache.end())
    {
        *comment = found->second;
        return true;
    }
    SHARED_RELEASE();

    duint data = 0;
    memset(comment, 0, sizeof(STACK_COMMENT));
    MemRead(addr, &data, sizeof(duint));
    if(!MemIsValidReadPtr(data)) //the stack value is no pointer
        return false;

    duint size = 0;
    duint base = MemFindBaseAddr(data, &size);
    duint readStart = data - 16 * 4;
    if(readStart < base)
        readStart = base;
    unsigned char disasmData[256];
    MemRead(readStart, disasmData, sizeof(disasmData));
    duint prev = disasmback(disasmData, 0, sizeof(disasmData), data - readStart, 1);
    duint previousInstr = readStart + prev;

    BASIC_INSTRUCTION_INFO basicinfo;
    bool valid = disasmfast(disasmData + prev, previousInstr, &basicinfo);
    if(valid && basicinfo.call) //call
    {
        char returnToAddr[MAX_COMMENT_SIZE] = "";
        getSymAddrName(data, returnToAddr);

        data = basicinfo.addr;
        char returnFromAddr[MAX_COMMENT_SIZE] = "";
        getSymAddrName(data, returnFromAddr);
        sprintf_s(comment->comment, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "return to %s from %s")), returnToAddr, returnFromAddr);
        strcpy_s(comment->color, "!rtnclr"); // Special token for return address color;
        return true;
    }

    //string
    char string[MAX_STRING_SIZE] = "";
    if(DbgGetStringAt(data, string))
    {
        strncpy_s(comment->comment, string, _TRUNCATE);
        return true;
    }

    //label
    char label[MAX_LABEL_SIZE] = "";
    ADDRINFO addrinfo;
    addrinfo.flags = flaglabel;
    if(_dbg_addrinfoget(data, SEG_DEFAULT, &addrinfo))
        strcpy_s(label, addrinfo.label);
    char module[MAX_MODULE_SIZE] = "";
    ModNameFromAddr(data, module, false);

    if(*module) //module
    {
        if(*label) //+label
            sprintf_s(comment->comment, "%s.%s", module, label);
        else //module only
            sprintf_s(comment->comment, "%s.%p", module, data);
        return true;
    }
    else if(*label) //label only
    {
        sprintf_s(comment->comment, "<%s>", label);
        return true;
    }

    return false;
}

static BOOL CALLBACK StackReadProcessMemoryProc64(HANDLE hProcess, DWORD64 lpBaseAddress, PVOID lpBuffer, DWORD nSize, LPDWORD lpNumberOfBytesRead)
{
    // Fix for 64-bit sizes
    SIZE_T bytesRead = 0;

    if(MemRead((duint)lpBaseAddress, lpBuffer, nSize, &bytesRead))
    {
        if(lpNumberOfBytesRead)
            *lpNumberOfBytesRead = (DWORD)bytesRead;

        return true;
    }

    return false;
}

static DWORD64 CALLBACK StackGetModuleBaseProc64(HANDLE hProcess, DWORD64 Address)
{
    return (DWORD64)ModBaseFromAddr((duint)Address);
}

static DWORD64 CALLBACK StackTranslateAddressProc64(HANDLE hProcess, HANDLE hThread, LPADDRESS64 lpaddr)
{
    ASSERT_ALWAYS("This function should never be called");
    return 0;
}

void StackEntryFromFrame(CALLSTACKENTRY* Entry, duint Address, duint From, duint To)
{
    Entry->addr = Address;
    Entry->from = From;
    Entry->to = To;

    char returnToAddr[MAX_COMMENT_SIZE] = "";
    getSymAddrName(To, returnToAddr);
    char returnFromAddr[MAX_COMMENT_SIZE] = "";
    getSymAddrName(From, returnFromAddr);
    sprintf_s(Entry->comment, GuiTranslateText(QT_TRANSLATE_NOOP("DBG", "return to %s from %s")), returnToAddr, returnFromAddr);
}

#define MAX_CALLSTACK_CACHE 20
using CallstackMap = std::unordered_map<duint, std::vector<CALLSTACKENTRY>>;
static CallstackMap CallstackCache;

void stackupdatecallstack(duint csp)
{
    std::vector<CALLSTACKENTRY> callstack;
    stackgetcallstack(csp, callstack, false);
}

void stackgetcallstack(duint csp, std::vector<CALLSTACKENTRY> & callstackVector, bool cache)
{
    if(cache)
    {
        SHARED_ACQUIRE(LockCallstackCache);
        auto found = CallstackCache.find(csp);
        if(found != CallstackCache.end())
        {
            callstackVector = found->second;
            return;
        }
        callstackVector.clear();
        return;
    }

    // Gather context data
    CONTEXT context;
    memset(&context, 0, sizeof(CONTEXT));

    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;

    if(SuspendThread(hActiveThread) == -1)
        return;

    if(!GetThreadContext(hActiveThread, &context))
        return;

    if(ResumeThread(hActiveThread) == -1)
        return;

    // Set up all frame data
    STACKFRAME64 frame;
    ZeroMemory(&frame, sizeof(STACKFRAME64));

#ifdef _M_IX86
    DWORD machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = csp;
    frame.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rsp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = csp;
    frame.AddrStack.Mode = AddrModeFlat;
#endif

    const int MaxWalks = 50;
    // Container for each callstack entry (50 pre-allocated entries)
    callstackVector.clear();
    callstackVector.reserve(MaxWalks);

    for(auto i = 0; i < MaxWalks; i++)
    {
        if(!StackWalk64(
                    machineType,
                    fdProcessInfo->hProcess,
                    hActiveThread,
                    &frame,
                    &context,
                    StackReadProcessMemoryProc64,
                    SymFunctionTableAccess64,
                    StackGetModuleBaseProc64,
                    StackTranslateAddressProc64))
        {
            // Maybe it failed, maybe we have finished walking the stack
            break;
        }

        if(frame.AddrPC.Offset != 0)
        {
            // Valid frame
            CALLSTACKENTRY entry;
            memset(&entry, 0, sizeof(CALLSTACKENTRY));

            StackEntryFromFrame(&entry, (duint)frame.AddrFrame.Offset + sizeof(duint), (duint)frame.AddrPC.Offset, (duint)frame.AddrReturn.Offset);
            callstackVector.push_back(entry);
        }
        else
        {
            // Base reached
            break;
        }
    }

    EXCLUSIVE_ACQUIRE(LockCallstackCache);
    if(CallstackCache.size() > MAX_CALLSTACK_CACHE)
        CallstackCache.clear();
    CallstackCache[csp] = callstackVector;
}

void stackgetcallstack(duint csp, CALLSTACK* callstack)
{
    std::vector<CALLSTACKENTRY> callstackVector;
    stackgetcallstack(csp, callstackVector, true);

    // Convert to a C data structure
    callstack->total = (int)callstackVector.size();

    if(callstack->total > 0)
    {
        callstack->entries = (CALLSTACKENTRY*)BridgeAlloc(callstack->total * sizeof(CALLSTACKENTRY));

        // Copy data directly from the vector
        memcpy(callstack->entries, callstackVector.data(), callstack->total * sizeof(CALLSTACKENTRY));
    }
}