/*
 * 386-specific Win32 dll<->dll snooping functions
 *
 * Copyright 1998 Marcus Meissner
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "winbase.h"
#include "winnt.h"
#include "winternl.h"
#include "snoop.h"
#include "stackframe.h"
#include "wine/debug.h"
#include "wine/exception.h"
#include "excpt.h"

WINE_DEFAULT_DEBUG_CHANNEL(snoop);

static WINE_EXCEPTION_FILTER(page_fault)
{
    if (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
        GetExceptionCode() == EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_EXECUTE_HANDLER;
    return EXCEPTION_CONTINUE_SEARCH;
}

extern const char **debug_snoop_excludelist;
extern const char **debug_snoop_includelist;

#ifdef __i386__

extern void WINAPI SNOOP_Entry();
extern void WINAPI SNOOP_Return();

#include "pshpack1.h"

typedef	struct tagSNOOP_FUN {
	/* code part */
	BYTE		lcall;		/* 0xe8 call snoopentry (relative) */
	/* NOTE: If you move snoopentry OR nrofargs fix the relative offset
	 * calculation!
	 */
	DWORD		snoopentry;	/* SNOOP_Entry relative */
	/* unreached */
	int		nrofargs;
	FARPROC	origfun;
	char		*name;
} SNOOP_FUN;

typedef struct tagSNOOP_DLL {
	HMODULE	hmod;
	SNOOP_FUN	*funs;
	DWORD		ordbase;
	DWORD		nrofordinals;
	struct tagSNOOP_DLL	*next;
	char name[1];
} SNOOP_DLL;

typedef struct tagSNOOP_RETURNENTRY {
	/* code part */
	BYTE		lcall;		/* 0xe8 call snoopret relative*/
	/* NOTE: If you move snoopret OR origreturn fix the relative offset
	 * calculation!
	 */
	DWORD		snoopret;	/* SNOOP_Ret relative */
	/* unreached */
	FARPROC	origreturn;
	SNOOP_DLL	*dll;
	DWORD		ordinal;
	DWORD		origESP;
	DWORD		*args;		/* saved args across a stdcall */
} SNOOP_RETURNENTRY;

typedef struct tagSNOOP_RETURNENTRIES {
	SNOOP_RETURNENTRY entry[4092/sizeof(SNOOP_RETURNENTRY)];
	struct tagSNOOP_RETURNENTRIES	*next;
} SNOOP_RETURNENTRIES;

#include "poppack.h"

static	SNOOP_DLL		*firstdll = NULL;
static	SNOOP_RETURNENTRIES 	*firstrets = NULL;

/***********************************************************************
 *          SNOOP_ShowDebugmsgSnoop
 *
 * Simple function to decide if a particular debugging message is
 * wanted.
 */
int SNOOP_ShowDebugmsgSnoop(const char *dll, int ord, const char *fname) {

  if(debug_snoop_excludelist || debug_snoop_includelist) {
    const char **listitem;
    char buf[80];
    int len, len2, itemlen, show;

    if(debug_snoop_excludelist) {
      show = 1;
      listitem = debug_snoop_excludelist;
    } else {
      show = 0;
      listitem = debug_snoop_includelist;
    }
    len = strlen(dll);
    assert(len < 64);
    sprintf(buf, "%s.%d", dll, ord);
    len2 = strlen(buf);
    for(; *listitem; listitem++) {
      itemlen = strlen(*listitem);
      if((itemlen == len && !strncasecmp(*listitem, buf, len)) ||
         (itemlen == len2 && !strncasecmp(*listitem, buf, len2)) ||
         !strcasecmp(*listitem, fname)) {
        show = !show;
       break;
      }
    }
    return show;
  }
  return 1;
}

void
SNOOP_RegisterDLL(HMODULE hmod,LPCSTR name,DWORD ordbase,DWORD nrofordinals) {
	SNOOP_DLL	**dll = &(firstdll);
	char		*s;

    TRACE("hmod=%p, name=%s, ordbase=%ld, nrofordinals=%ld\n",
	   hmod, name, ordbase, nrofordinals);

	if (!TRACE_ON(snoop)) return;
	while (*dll) {
		if ((*dll)->hmod == hmod)
		{
		    /* another dll, loaded at the same address */
		    VirtualFree((*dll)->funs, (*dll)->nrofordinals*sizeof(SNOOP_FUN), MEM_RELEASE);
		    break;
		}
		dll = &((*dll)->next);
	}
        *dll = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *dll, sizeof(SNOOP_DLL)+strlen(name));
	(*dll)->hmod	= hmod;
	(*dll)->ordbase = ordbase;
	(*dll)->nrofordinals = nrofordinals;
	strcpy( (*dll)->name, name );
	if ((s=strrchr((*dll)->name,'.')))
		*s='\0';
	(*dll)->funs = VirtualAlloc(NULL,nrofordinals*sizeof(SNOOP_FUN),MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
	memset((*dll)->funs,0,nrofordinals*sizeof(SNOOP_FUN));
	if (!(*dll)->funs) {
		HeapFree(GetProcessHeap(),0,*dll);
		FIXME("out of memory\n");
		return;
	}
}

FARPROC
SNOOP_GetProcAddress(HMODULE hmod,LPCSTR name,DWORD ordinal,FARPROC origfun) {
	SNOOP_DLL			*dll = firstdll;
	SNOOP_FUN			*fun;
        IMAGE_SECTION_HEADER *sec;

	if (!TRACE_ON(snoop)) return origfun;
	if (!*(LPBYTE)origfun) /* 0x00 is an imposs. opcode, poss. dataref. */
		return origfun;

        sec = RtlImageRvaToSection( RtlImageNtHeader(hmod), hmod, (char *)origfun - (char *)hmod );

        if (!sec || !(sec->Characteristics & IMAGE_SCN_CNT_CODE))
            return origfun;  /* most likely a data reference */

	while (dll) {
		if (hmod == dll->hmod)
			break;
		dll=dll->next;
	}
	if (!dll)	/* probably internal */
		return origfun;
	if (!SNOOP_ShowDebugmsgSnoop(dll->name,ordinal,name))
		return origfun;
	assert(ordinal < dll->nrofordinals);
	fun = dll->funs+ordinal;
	if (!fun->name)
	  {
	    fun->name = HeapAlloc(GetProcessHeap(),0,strlen(name)+1);
	    strcpy( fun->name, name );
	    fun->lcall	= 0xe8;
	    /* NOTE: origreturn struct member MUST come directly after snoopentry */
	    fun->snoopentry	= (char*)SNOOP_Entry-((char*)(&fun->nrofargs));
	    fun->origfun	= origfun;
	    fun->nrofargs	= -1;
	  }
	return (FARPROC)&(fun->lcall);
}

static void SNOOP_PrintArg(DWORD x)
{
    int i,nostring;

    DPRINTF("%08lx",x);
    if ( !HIWORD(x) ) return; /* trivial reject to avoid faults */
    __TRY
    {
        LPBYTE s=(LPBYTE)x;
        i=0;nostring=0;
        while (i<80) {
            if (s[i]==0) break;
            if (s[i]<0x20) {nostring=1;break;}
            if (s[i]>=0x80) {nostring=1;break;}
            i++;
        }
        if (!nostring && i > 5)
            DPRINTF(" %s",debugstr_an((LPSTR)x,i));
        else  /* try unicode */
        {
            LPWSTR s=(LPWSTR)x;
            i=0;nostring=0;
            while (i<80) {
                if (s[i]==0) break;
                if (s[i]<0x20) {nostring=1;break;}
                if (s[i]>0x100) {nostring=1;break;}
                i++;
            }
            if (!nostring && i > 5) DPRINTF(" %s",debugstr_wn((LPWSTR)x,i));
        }
    }
    __EXCEPT(page_fault)
    {
    }
    __ENDTRY
}

#define CALLER1REF (*(DWORD*)context->Esp)

void WINAPI SNOOP_DoEntry( CONTEXT86 *context )
{
	DWORD		ordinal=0,entry = context->Eip - 5;
	SNOOP_DLL	*dll = firstdll;
	SNOOP_FUN	*fun = NULL;
	SNOOP_RETURNENTRIES	**rets = &firstrets;
	SNOOP_RETURNENTRY	*ret;
	int		i=0, max;

	while (dll) {
		if (	((char*)entry>=(char*)dll->funs)	&&
			((char*)entry<=(char*)(dll->funs+dll->nrofordinals))
		) {
			fun = (SNOOP_FUN*)entry;
			ordinal = fun-dll->funs;
			break;
		}
		dll=dll->next;
	}
	if (!dll) {
		FIXME("entrypoint 0x%08lx not found\n",entry);
		return; /* oops */
	}
	/* guess cdecl ... */
	if (fun->nrofargs<0) {
		/* Typical cdecl return frame is:
		 * 	add esp, xxxxxxxx
		 * which has (for xxxxxxxx up to 255 the opcode "83 C4 xx".
		 * (after that 81 C2 xx xx xx xx)
		 */
		LPBYTE	reteip = (LPBYTE)CALLER1REF;

		if (reteip) {
			if ((reteip[0]==0x83)&&(reteip[1]==0xc4))
				fun->nrofargs=reteip[2]/4;
		}
	}


	while (*rets) {
		for (i=0;i<sizeof((*rets)->entry)/sizeof((*rets)->entry[0]);i++)
			if (!(*rets)->entry[i].origreturn)
				break;
		if (i!=sizeof((*rets)->entry)/sizeof((*rets)->entry[0]))
			break;
		rets = &((*rets)->next);
	}
	if (!*rets) {
		*rets = VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
		memset(*rets,0,4096);
		i = 0;	/* entry 0 is free */
	}
	ret = &((*rets)->entry[i]);
	ret->lcall	= 0xe8;
	/* NOTE: origreturn struct member MUST come directly after snoopret */
	ret->snoopret	= ((char*)SNOOP_Return)-(char*)(&ret->origreturn);
	ret->origreturn	= (FARPROC)CALLER1REF;
	CALLER1REF	= (DWORD)&ret->lcall;
	ret->dll	= dll;
	ret->args	= NULL;
	ret->ordinal	= ordinal;
	ret->origESP	= context->Esp;

	context->Eip = (DWORD)fun->origfun;

	DPRINTF("%08lx:CALL %s.%ld: %s(",GetCurrentThreadId(),dll->name,dll->ordbase+ordinal,fun->name);
	if (fun->nrofargs>0) {
		max = fun->nrofargs; if (max>16) max=16;
		for (i=0;i<max;i++)
                {
                    SNOOP_PrintArg(*(DWORD*)(context->Esp + 4 + sizeof(DWORD)*i));
                    if (i<fun->nrofargs-1) DPRINTF(",");
                }
		if (max!=fun->nrofargs)
			DPRINTF(" ...");
	} else if (fun->nrofargs<0) {
		DPRINTF("<unknown, check return>");
		ret->args = HeapAlloc(GetProcessHeap(),0,16*sizeof(DWORD));
		memcpy(ret->args,(LPBYTE)(context->Esp + 4),sizeof(DWORD)*16);
	}
	DPRINTF(") ret=%08lx\n",(DWORD)ret->origreturn);
}


void WINAPI SNOOP_DoReturn( CONTEXT86 *context )
{
	SNOOP_RETURNENTRY	*ret = (SNOOP_RETURNENTRY*)(context->Eip - 5);

	/* We haven't found out the nrofargs yet. If we called a cdecl
	 * function it is too late anyway and we can just set '0' (which
	 * will be the difference between orig and current ESP
	 * If stdcall -> everything ok.
	 */
	if (ret->dll->funs[ret->ordinal].nrofargs<0)
		ret->dll->funs[ret->ordinal].nrofargs=(context->Esp - ret->origESP-4)/4;
	context->Eip = (DWORD)ret->origreturn;
	if (ret->args) {
		int	i,max;

		DPRINTF("%08lx:RET  %s.%ld: %s(",
		        GetCurrentThreadId(),
		        ret->dll->name,ret->dll->ordbase+ret->ordinal,ret->dll->funs[ret->ordinal].name);
		max = ret->dll->funs[ret->ordinal].nrofargs;
		if (max>16) max=16;

		for (i=0;i<max;i++)
                {
                    SNOOP_PrintArg(ret->args[i]);
                    if (i<max-1) DPRINTF(",");
                }
		DPRINTF(") retval = %08lx ret=%08lx\n",
			context->Eax,(DWORD)ret->origreturn );
		HeapFree(GetProcessHeap(),0,ret->args);
		ret->args = NULL;
	} else
		DPRINTF("%08lx:RET  %s.%ld: %s() retval = %08lx ret=%08lx\n",
			GetCurrentThreadId(),
			ret->dll->name,ret->dll->ordbase+ret->ordinal,ret->dll->funs[ret->ordinal].name,
			context->Eax, (DWORD)ret->origreturn);
	ret->origreturn = NULL; /* mark as empty */
}

/* assembly wrappers that save the context */
__ASM_GLOBAL_FUNC( SNOOP_Entry,
                   "call " __ASM_NAME("__wine_call_from_32_regs") "\n\t"
                   ".long " __ASM_NAME("SNOOP_DoEntry") ",0" );
__ASM_GLOBAL_FUNC( SNOOP_Return,
                   "call " __ASM_NAME("__wine_call_from_32_regs") "\n\t"
                   ".long " __ASM_NAME("SNOOP_DoReturn") ",0" );

#else	/* !__i386__ */
void SNOOP_RegisterDLL(HMODULE hmod,LPCSTR name,DWORD nrofordinals, DWORD dw) {
	if (!TRACE_ON(snoop)) return;
	FIXME("snooping works only on i386 for now.\n");
}

FARPROC SNOOP_GetProcAddress(HMODULE hmod,LPCSTR name,DWORD ordinal,FARPROC origfun) {
	return origfun;
}
#endif	/* !__i386__ */
