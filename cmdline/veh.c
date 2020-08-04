/**
  Copyright © 2020 Odzhan. All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  1. Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  3. The name of the author may not be used to endorse or promote products
  derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY AUTHORS "AS IS" AND ANY EXPRESS OR
  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
  STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE. */
  
#define UNICODE
#include "../ntlib/util.h"

typedef HRESULT(WINAPI *_RtlDecodeRemotePointer)(
  HANDLE    ProcessHandle,
  PVOID     Ptr,
  PVOID     *DecodedPtr);
  
// vectored handler list
typedef struct _RTL_VECTORED_HANDLER_LIST {
    PSRWLOCK                    Lock;
    LIST_ENTRY                  List;
} RTL_VECTORED_HANDLER_LIST, *PRTL_VECTORED_HANDLER_LIST;

// exception handler entry
typedef struct _RTL_VECTORED_EXCEPTION_ENTRY {
    LIST_ENTRY                  List;
    PULONG_PTR                  Flag;           // some flag related to CFG
    ULONG                       RefCount;
    PVECTORED_EXCEPTION_HANDLER VectoredHandler;
} RTL_VECTORED_EXCEPTION_ENTRY, *PRTL_VECTORED_EXCEPTION_ENTRY;

// fake handler
LONG WINAPI VectoredHandler(struct _EXCEPTION_POINTERS *ExceptionInfo) {
    UNREFERENCED_PARAMETER(ExceptionInfo);
    
    return EXCEPTION_CONTINUE_SEARCH;
}

// search the .mrdata section for fake handler
PVOID GetVectoredHandlerList(VOID) {
    PIMAGE_DOS_HEADER      dos;
    PIMAGE_NT_HEADERS      nt;
    PIMAGE_SECTION_HEADER  sh;
    HMODULE                m;
    PVOID                  h;
    BOOL                   found = FALSE;
    DWORD                  i, cnt;
    PULONG_PTR             ds;
    
    // install our own handler.
    h = AddVectoredExceptionHandler(1, VectoredHandler);
    
    // now try find in list
    m   = GetModuleHandle(L"ntdll");
    dos = (PIMAGE_DOS_HEADER)m;  
    nt  = RVA2VA(PIMAGE_NT_HEADERS, m, dos->e_lfanew);  
    sh  = (PIMAGE_SECTION_HEADER)((LPBYTE)&nt->OptionalHeader + 
            nt->FileHeader.SizeOfOptionalHeader);
    
    // locate the .data segment, save VA and number of pointers
    for(i=0; i<nt->FileHeader.NumberOfSections; i++) {
      if(*(PDWORD)sh[i].Name == *(PDWORD)".mrdata") {
        ds  = RVA2VA(PULONG_PTR, m, sh[i].VirtualAddress);
        cnt = sh[i].Misc.VirtualSize / sizeof(ULONG_PTR);
        break;
      }
    }

    // Find handler in section
    for(i=0; i<cnt - 1 && !(found = ((PVOID)ds[i] == h)); i++);
    
    // remove handler from list
    RemoveVectoredExceptionHandler(h);
    
    // if found, return the pointer to list
    return found ? &ds[i - 1] : NULL;    
}

void veh_dump(HANDLE hp, PWCHAR proc, PVOID vhl_va, int idx) {
    RTL_VECTORED_EXCEPTION_ENTRY ee;
    RTL_VECTORED_HANDLER_LIST    vhl[2];
    SIZE_T                       rd;
    PVOID                        ptr;
    _RtlDecodeRemotePointer      RtlDecodeRemotePointer;
    
    RtlDecodeRemotePointer = (_RtlDecodeRemotePointer)
      GetProcAddress(GetModuleHandle(L"ntdll"), "RtlDecodeRemotePointer");
      
    // read list
    ReadProcessMemory(
      hp, vhl_va, &vhl, sizeof(vhl), &rd);
    
    ptr = vhl[idx].List.Flink;
    
    for(;;) {
      // read entry
      ReadProcessMemory(
        hp, ptr, &ee, sizeof(ee), &rd);
      
      if(ee.List.Flink == vhl[idx].List.Flink) break;
      
      RtlDecodeRemotePointer(hp, ee.VectoredHandler, &ptr);
      wprintf(L"VEH | %-25s : %p\n", proc, ptr);

      ptr = ee.List.Flink;
    }
}

void seh_dump(HANDLE hp, DWORD pid, PWCHAR proc) {
    HANDLE                   ss, ht;
    THREADENTRY32            te;
    NTSTATUS                 nts;
    PVOID                    el;
    SIZE_T                   rd;
    THREAD_BASIC_INFORMATION tbi;
    ULONG                    len;
    
    // 1. Enumerate threads in target process
    ss = CreateToolhelp32Snapshot(
      TH32CS_SNAPTHREAD, 0);
      
    if(ss == INVALID_HANDLE_VALUE) return;

    te.dwSize = sizeof(THREADENTRY32);
    
    if(Thread32First(ss, &te)) {
      do {
        // if not our target process, skip it
        if(te.th32OwnerProcessID != pid) continue;
        
        // if we can't open thread, skip it
        ht = OpenThread(
          THREAD_ALL_ACCESS, 
          FALSE, 
          te.th32ThreadID);
          
        if(ht == NULL) continue;
        
        nts = NtQueryInformationThread(
          ht, ThreadBasicInformation,
          &tbi, sizeof(tbi), &len);
        
        if(nts == 0) {
          ReadProcessMemory(hp, 
            tbi.TebBaseAddress, &el, 
            sizeof(ULONG_PTR), &rd);
          
          if(el != NULL) 
            wprintf(L"SEH | %-25s : %p\n", proc, el);
        }
        CloseHandle(ht);
      } while(Thread32Next(ss, &te));
    }
    CloseHandle(ss);
}
    
void scan_system(DWORD pid) {
    PVOID          va;
    HANDLE         ss;
    PROCESSENTRY32 pe;
    HANDLE         hp;
    
    va = GetVectoredHandlerList();
    
    if(va == NULL) {
      wprintf(L"  [ ERROR: Unable to resolve address of LdrpVectorHandlerList.\n");
      return;
    }
    
    ss = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(ss == INVALID_HANDLE_VALUE) return;
    
    pe.dwSize = sizeof(PROCESSENTRY32);

    if(Process32First(ss, &pe)){
      do {
        // skip system
        if(pe.th32ProcessID <= 4) continue;
        
        // if filtering by process id, skip entries that don't match
        if(pid != 0 && pe.th32ProcessID != pid) continue;
        
        // try open process
        hp = OpenProcess(
          PROCESS_ALL_ACCESS, 
          FALSE, 
          pe.th32ProcessID);
          
        if(hp != NULL) {
          seh_dump(hp, pe.th32ProcessID, pe.szExeFile);
          
          veh_dump(hp, pe.szExeFile, va, 0);
          veh_dump(hp, pe.szExeFile, va, 1);
          
          CloseHandle(hp);
        }
      } while(Process32Next(ss, &pe));
    }
    CloseHandle(ss);
}

int main(void) {
    WCHAR **argv, *process=NULL;
    int   argc, pid=0;
    
    argv = CommandLineToArgvW(GetCommandLine(), &argc);
    
    if(argc == 2) {
      pid = name2pid(argv[1]);
      if(pid == 0) pid = wcstoull(argv[1], NULL, 10);
      if(pid == 0) {
        wprintf(L"  [ ERROR: Unable to resolve pid for \"%s\".\n", argv[1]);
        return -1;
      }
    }
    
    // try enable debug privilege
    if(!SetPrivilege(SE_DEBUG_NAME, TRUE)) {
      wprintf(L"  [ WARNING: Failed to enable debugging privilege.\n");
    }
    
    scan_system(pid);
    
    return 0;
}
