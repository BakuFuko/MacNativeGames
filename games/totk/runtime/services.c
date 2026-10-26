/* Horizon services reached by the current TOTK startup. Unknown operations fail. */
#include "runtime.h"
#include <TargetConditionals.h>
#include "ipc.h"
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <ctype.h>
#include <string.h>
#include <sys/mman.h>


#include <time.h>
#include <unistd.h>
#include <mach/mach_time.h>
/* Guest address layout. The SDK encodes IPC buffer addresses with 39 bits (HIPC descriptor bits 36..38),
   so all guest memory must lie below 2^39. Darwin arm64 keeps 0x10_0000_0000..0x70_0000_0000 reserved,
   leaving exactly 64 GiB at 0x70_0000_0000 for the alias region (the real 39-bit Horizon size, which the
   game's allocators depend on); the stack and heap regions are reserved separately below it. */
#define GUEST_ADDRESS_LIMIT (1ULL<<39)
#define ALIAS_SPACE_AT 0x7000000000ULL
#define ALIAS_SIZE_MAX (64ULL<<30)
/* TOTK_ALIAS_GIB: alias region size reported to the game (default 64, the real 39-bit Horizon size). An iOS app signed
   without extended virtual addressing (unavailable to personal teams) gets a much smaller map: the old 6 + 16 GiB
   reservation failed on an iPhone, while 4.75 + 16 GiB fit on an iPad (A16, iPadOS 26.7). So the largest size from 16
   down to 4 GiB that fits is chosen at startup; the game maps about 3.3 GiB of it. */
#if TARGET_OS_IPHONE
#define ALIAS_DEFAULT_GIB 16
#define ALIAS_MIN_GIB 4ULL
#else
#define ALIAS_DEFAULT_GIB 64
#endif
static uint64_t alias_len;
static uint64_t alias_requested(void){const char *e=getenv("TOTK_ALIAS_GIB");uint64_t g=e?strtoull(e,NULL,0):ALIAS_DEFAULT_GIB;if(g<1||g>64)runtime_fail("TOTK_ALIAS_GIB must be 1..64");return g<<30;}
static uint64_t alias_size(void){if(!alias_len)alias_len=alias_requested();return alias_len;}
#define ALIAS_SIZE alias_size()
#define LOW_SEARCH_FROM 0x300000000ULL
#if TARGET_OS_IPHONE
#define LOW_SIZE (HEAP_OFF+HEAP_CAP)   /* iOS: no slack after the heap cap; address space is scarce */
#else
#define LOW_SIZE (6ULL<<30)
#endif
#define STACK_OFF 0x10000000ULL
#define STACK_SIZE 0x10000000ULL
#define HEAP_OFF (STACK_OFF+STACK_SIZE+0x10000000ULL)
static uint8_t *alias_space,*low_space,*heap;
static size_t heap_size;
static unsigned char alias_pages[ALIAS_SIZE_MAX/4096];
/* 4 KiB guest pages that are unmapped but whose host 16 KiB page is still committed with old contents: only these are
   zeroed when mapped again. Every other free page is untouched anonymous memory (zero on first touch), so mapping does
   not make it resident; a fully free 16 KiB host page is handed back to the system (fresh anonymous mapping). On the
   iPhone, where the app may use about 4 GB in total, memory the game maps but never touches must stay unbacked. */
static unsigned char alias_stale[ALIAS_SIZE_MAX/4096];
static uint64_t alias_bytes;
static uint8_t user_exception_context[0x1000] __attribute__((aligned(0x1000)));
#define SYSTEM_RESOURCE_BUDGET (32ULL<<20)
#ifdef GAME_BOTW
#define GAME_PROGRAM_ID 0x01007ef00011e000ULL   /* GetInfo 18 (ProgramId): Breath of the Wild */
#else
#define GAME_PROGRAM_ID 0x0100f2c0115b6000ULL
#endif
#if TARGET_OS_IPHONE
#define HEAP_CAP (3ULL<<30)   /* iOS: shaved 1 GiB off so stack+heap+alias fit the scarce vm map (1.4.3 asks ~2.2 GiB) */
#else
#define HEAP_CAP (4ULL<<30)   /* 1.4.3 sets a 2.2 GiB heap during nninitStartup */
#endif
#define APP_POOL app_pool()
static uint64_t app_pool(void){static uint64_t v;if(!v){const char *e=getenv("TOTK_APP_POOL");uint64_t extra=(runtime_output_res_extra_bytes()+0x1fffff)&~0x1fffffULL;
 v=e?strtoull(e,NULL,0):0xcd500000ULL+extra;if(!e&&extra)fprintf(stderr,"[totk] application pool %#llx: +%llu MiB for the bigger texture heaps (TOTK_OUTPUT_RES, TOTK_RENDER_RES)\n",(unsigned long long)v,(unsigned long long)(extra>>20));}return v;}
/* TOTK: application memory pool reported to the game (default: retail 3.2 GiB; TOTK_OUTPUT_RES adds what its HeapDef overlay gives the texture heaps) */
#define ALIAS_START ((uintptr_t)alias_space)
#define STACK_START ((uintptr_t)low_space+STACK_OFF)
#define ASLR_START ((uintptr_t)low_space)
#if TARGET_OS_IPHONE
#define ASLR_SIZE (ALIAS_START+ALIAS_SIZE-(uintptr_t)low_space)   /* iOS: everything the process can map ends far below 2^39 */
#else
#define ASLR_SIZE (GUEST_ADDRESS_LIMIT-(uintptr_t)low_space)
#endif
static uint8_t *reserve_fixed(uint64_t at,uint64_t size){
 mach_vm_address_t a=at;   /* FIXED without OVERWRITE: fails instead of replacing an existing mapping */
 return mach_vm_map(mach_task_self(),&a,size,0,VM_FLAGS_FIXED,MEMORY_OBJECT_NULL,0,FALSE,VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_DEFAULT)==KERN_SUCCESS?(uint8_t *)(uintptr_t)a:NULL;
}
#define GUEST_ALIGN (2ULL<<20)
/* Reserve size bytes in a free gap (2 MiB aligned). The end of the map is
   not reported, so the last gap is only tried; FIXED fails there with KERN_NO_SPACE when it reaches past the end. */
static uint8_t *reserve_in_gap_from(uint64_t size,int log,uint64_t prev){
 for(;;){
  mach_vm_address_t r=prev;mach_vm_size_t rsize=0;vm_region_basic_info_data_64_t info;mach_msg_type_number_t n=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t obj=MACH_PORT_NULL;
  int more=mach_vm_region(mach_task_self(),&r,&rsize,VM_REGION_BASIC_INFO_64,(vm_region_info_t)&info,&n,&obj)==KERN_SUCCESS;
  if(obj!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),obj);
  uint64_t start=(prev+GUEST_ALIGN-1)&~(GUEST_ALIGN-1),end=more&&r<GUEST_ADDRESS_LIMIT?r:GUEST_ADDRESS_LIMIT;
  if(end>start&&end-start>=(256ULL<<20)&&log)fprintf(stderr,"[totk-mem] free %#llx..%s%#llx\n",(unsigned long long)start,more?"":"(end of map, at most) ",(unsigned long long)end);
  if(end>start&&end-start>=size){uint8_t *p=reserve_fixed(start,size);if(p)return p;}
  if(!more||r>=GUEST_ADDRESS_LIMIT||!rsize||r+rsize<=prev)return NULL;
  prev=r+rsize;
 }
}
static uint8_t *reserve_in_gap(uint64_t size,int log){
#if TARGET_OS_IPHONE
 return reserve_in_gap_from(size,log,1ULL<<32);
#else
 return reserve_in_gap_from(size,log,LOW_SEARCH_FROM);
#endif
}
static void memory_init(void) {
 if(alias_space)return;
#if TARGET_OS_IPHONE
 /* iOS: stack+heap and the alias region go side by side in one reservation (ASLR_SIZE spans both), placed in the lowest
    free gap that holds them. The free space depends on the device, so the alias region shrinks from the requested size
    to ALIAS_MIN_GIB before giving up (logging the free gaps); TOTK_ALIAS_GIB pins one size. */
 uint64_t want=alias_requested(),least=getenv("TOTK_ALIAS_GIB")?want:(ALIAS_MIN_GIB<<30);
 for(uint64_t a=want;a>=least&&!low_space;a-=1ULL<<30)if((low_space=reserve_in_gap(LOW_SIZE+a,0)))alias_len=a;
 if(!low_space){
  reserve_in_gap(UINT64_MAX,1);
  {
   /* diagnostic: probe 0x70_0000_0000 with increasing sizes and print every attempt,
      so a failing startup reveals how much is actually mappable before the first failure
      (kr: 0=success, 1=KERN_INVALID_ADDRESS, 3=KERN_NO_SPACE). */
   uint64_t need=LOW_SIZE+alias_requested();
   for(uint64_t sz=1ULL<<30;sz<=need;sz+=1ULL<<30){
    mach_vm_address_t a=ALIAS_SPACE_AT;mach_vm_size_t n=sz;
    kern_return_t kr=mach_vm_map(mach_task_self(),&a,n,0,VM_FLAGS_FIXED,MEMORY_OBJECT_NULL,0,FALSE,VM_PROT_NONE,VM_PROT_ALL,VM_INHERIT_DEFAULT);
    fprintf(stderr,"[totk-diag] FIXED %#llx+%#llx: kr=%d\n",(unsigned long long)ALIAS_SPACE_AT,(unsigned long long)sz,kr);
    if(kr!=KERN_SUCCESS)break;
    mach_vm_deallocate(mach_task_self(),a,n);
   }
  }
  runtime_fail("reserve guest stack, heap and alias regions");
 }
 alias_space=low_space+LOW_SIZE;
#else
 alias_space=reserve_fixed(ALIAS_SPACE_AT,ALIAS_SIZE);
 if(alias_space)for(uint64_t at=LOW_SEARCH_FROM;at+LOW_SIZE<=ALIAS_SPACE_AT&&!low_space;at+=1ULL<<30)low_space=reserve_fixed(at,LOW_SIZE);
 if(!low_space){
  /* macOS 27: the shared cache fills 0x1_8000_0000..0xF_C000_0000 and the system reserves ~24 GiB at a random place in
     0x70_0000_0000..2^39, so neither fixed region fits. As on iOS, put stack+heap and the alias region side by side in one
     free hole, shrinking the alias region (the game uses < 3 GiB of it) until it fits. TOTK_ALIAS_GIB pins one size. */
 if(runtime_ipc_svc(num,r))return r[0];
 memory_init();
 switch(num) {
 case 0x02:r[0]=runtime_module_permission(r[0],r[1],(unsigned)r[2]);return r[0];
 case 0x03:r[0]=runtime_module_lock_permission(r[0],r[1],(unsigned)r[2],(unsigned)r[3]);return r[0];
 case 0x29: {
  uint64_t value=0;unsigned id=(unsigned)r[1];
  switch(id) {
  case 0:value=7;break;case 1:value=0x0ffffffff0000000ULL;break;
  case 2:value=ALIAS_START;break;case 3:value=ALIAS_SIZE;break;
  case 4:value=(uintptr_t)heap;break;case 5:value=HEAP_CAP;break;
  case 6:case 21:value=APP_POOL;break;case 7:case 22:value=runtime_image_bytes()+heap_size+alias_bytes;
   { static unsigned polls; if(getenv("TOTK_MEM_LOG")&&(polls++%600)==0) fprintf(stderr,"[totk-mem] GetInfo(%u) total=%#llx used=%#llx image=%#llx heap=%#llx alias=%#llx\n",id,(unsigned long long)APP_POOL,(unsigned long long)value,(unsigned long long)runtime_image_bytes(),(unsigned long long)heap_size,(unsigned long long)alias_bytes); }
   break;
  case 8:value=0;break;case 10:value=0;break;
  case 11:arc4random_buf(&value,8);break;
  case 12:value=ASLR_START;break;case 13:value=ASLR_SIZE;break;
  case 14:value=STACK_START;break;case 15:value=STACK_SIZE;break;
  case 16:value=SYSTEM_RESOURCE_BUDGET;break;case 17:value=sizeof(stack_maps)+sizeof(alias_pages);break;case 18:value=GAME_PROGRAM_ID;break;
  case 20:value=(uintptr_t)user_exception_context;break;   /* UserExceptionContextAddress: SDK stores e.g. the dying-message region at +0x1c0 */case 23:value=1;break;
  /* AliasRegionExtraSize (18.0.0+): our alias reservation contains no
   * kernel-owned extension outside the range reported by ids 2 and 3. */
  case 28:value=0;break;
  default:fprintf(stderr,"unknown GetInfo id %u\n",id);runtime_fail("GetInfo id not implemented");
  }
  r[0]=0;r[1]=value;return 0;
 }
 case 0x2c:case 0x2d:{uint64_t a=r[0],n=r[1];r[0]=alias_map(num==0x2d,a,n);
  if(getenv("TOTK_MEM_LOG"))fprintf(stderr,"[totk-mem] %sMapPhysicalMemory %#llx+%#llx -> %#llx alias=%#llx remaining=%#llx\n",num==0x2d?"Un":"",(unsigned long long)a,(unsigned long long)n,(unsigned long long)r[0],(unsigned long long)alias_bytes,(unsigned long long)(APP_POOL-runtime_image_bytes()-heap_size-alias_bytes));
  if(r[0])fprintf(stderr,"[totk] %sMapPhysicalMemory %#llx+%#llx failed %#llx (alias %#llx used, heap %#llx)\n",num==0x2d?"Un":"",(unsigned long long)a,(unsigned long long)n,(unsigned long long)r[0],(unsigned long long)alias_bytes,(unsigned long long)heap_size);return r[0];}
 case 0x04:case 0x05:r[0]=stack_move(num==5,r[0],r[1],r[2]);return r[0];
 case 0x06: {
  uint64_t addr=r[2],base=addr&~0xfffULL,size=0x1000;unsigned state=0,perm=0,attr=0;
  uintptr_t top=(uintptr_t)pthread_get_stackaddr_np(pthread_self());size_t stack=pthread_get_stacksize_np(pthread_self());
  if(addr>=top-stack&&addr<top){base=top-stack;size=stack;state=0xb;perm=3;}
  else if(addr>=(uintptr_t)heap&&addr<(uintptr_t)heap+heap_size){base=(uintptr_t)heap;size=heap_size;state=5;perm=3;}
  else if(addr>=(uintptr_t)low_space&&addr<(uintptr_t)low_space+LOW_SIZE){base=(uintptr_t)low_space;size=LOW_SIZE;if(addr<(uintptr_t)heap){size=(uintptr_t)heap-base;}else{base=(uintptr_t)heap+heap_size;size=(uintptr_t)low_space+LOW_SIZE-base;}}
  else if(addr>=(uintptr_t)low_space+LOW_SIZE&&addr<ALIAS_START){base=(uintptr_t)low_space+LOW_SIZE;size=ALIAS_START-base;state=0x10;perm=0;}   /* host-owned: Inaccessible */
  else if(!addr){base=0;size=ASLR_START;state=0;perm=0;}
  else if(runtime_module_query(addr,&base,&size,&state,&perm,&attr)){}
  else {state=3;perm=5;}
  pthread_mutex_lock(&memory_lock);
  if(addr>=ALIAS_START&&addr<ALIAS_START+ALIAS_SIZE){
   unsigned p=(unsigned)((addr-ALIAS_START)/4096),low=p,high=p+1;unsigned allocated=alias_pages[p];
   while(low>0&&alias_pages[low-1]==allocated)low--;while(high<sizeof(alias_pages)&&alias_pages[high]==allocated)high++;
   base=ALIAS_START+(uint64_t)low*4096;size=(uint64_t)(high-low)*4096;state=allocated?5:0;perm=allocated?3:0;
  }
  for(unsigned i=0;i<nstack_maps;i++){
   StackMap *m=&stack_maps[i];uint64_t end=m->dst+m->size,send=m->src+m->size;
   if(addr>=m->dst&&addr<end){base=m->dst;size=m->size;state=0xb;perm=3;break;}
   if(addr>=m->src&&addr<send){base=m->src;size=m->size;state=5;perm=0;attr=4;break;}
   if(state==0){uint64_t oldEnd=base+size;if(end<=addr&&end>base)base=end;if(m->dst>addr&&m->dst<oldEnd)oldEnd=m->dst;size=oldEnd-base;}
   else if(state==5){uint64_t oldEnd=base+size;if(send<=addr&&send>base)base=send;if(m->src>addr&&m->src<oldEnd)oldEnd=m->src;size=oldEnd-base;}
  }pthread_mutex_unlock(&memory_lock);
  struct {uint64_t base,size;uint32_t state,attr,perm,ipc,device,pad;} info={base,size,state,attr,perm,0,0,0};
  if(r[0])memcpy((void *)(uintptr_t)r[0],&info,sizeof(info));r[0]=0;r[1]=0;return 0;
 }
 case 0x01:{size_t size=r[1];if(getenv("TOTK_MEM_LOG"))fprintf(stderr,"[totk-mem] SetHeapSize %#zx (image=%#llx alias=%#llx pool=%#llx)\n",size,(unsigned long long)runtime_image_bytes(),(unsigned long long)alias_bytes,(unsigned long long)APP_POOL);
  if(size>HEAP_CAP||size%0x200000||size+runtime_image_bytes()>APP_POOL){r[0]=0xd401;return r[0];}
  if(size>heap_size&&mprotect(heap+heap_size,size-heap_size,PROT_READ|PROT_WRITE))runtime_fail("commit heap");
  if(size<heap_size&&mprotect(heap+size,heap_size-size,PROT_NONE))runtime_fail("decommit heap");heap_size=size;r[0]=0;r[1]=(uintptr_t)heap;return 0;}
 case 0x0c: r[0]=0;r[1]=44;return 0;
 case 0x0e: r[0]=0;r[1]=0;r[2]=15;return 0;
 case 0x25: {uint64_t id;pthread_threadid_np(NULL,&id);r[0]=0;r[1]=id;return 0;}
 case 0x10:r[0]=0;return 0;
 case 0x26: {uint32_t result=0;if(r[1]&&r[2]>=4)memcpy(&result,(void *)(uintptr_t)r[1],4);fprintf(stderr,"[totk] guest Break reason=%llx result=%x\n",(unsigned long long)r[0],result);runtime_trace_guest(r[29],r[30]);if(r[0]&0x80000000ULL)return 0;runtime_fail("original SDK Break");}
 case 0x27: {size_t n=r[1];if(n>65536)runtime_fail("debug string too long");fwrite((void *)(uintptr_t)r[0],1,n,stderr);r[0]=0;return 0;}
 case 0x0b:{int64_t ns=(int64_t)r[0];if(ns>0){struct timespec t={ns/1000000000,ns%1000000000};nanosleep(&t,NULL);}r[0]=0;return 0;}
 default:fprintf(stderr,"[totk] unimplemented SVC 0x%x at %s+%#llx\n",num,module,(unsigned long long)offset);if(num==0x1f)fprintf(stderr,"[totk] named port %.12s\n",(const char *)(uintptr_t)r[1]);runtime_trace_guest(r[29],r[30]);runtime_fail("system service not implemented");return 0;
 }
}
#ifdef GAME_BOTW
#undef runtime_svc
#endif
