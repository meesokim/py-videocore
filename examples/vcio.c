#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#define PHYS_REG_BASE    0x3F000000
#define GPIO_BASE       (PHYS_REG_BASE + 0x200000)
#define PAGE_SIZE       0x1000

// Videocore mailbox memory allocation flags, see:
//     https://github.com/raspberrypi/firmware/wiki/Mailbox-property-interface
typedef enum {
    MEM_FLAG_DISCARDABLE    = 1<<0, // can be resized to 0 at any time. Use for cached data
    MEM_FLAG_NORMAL         = 0<<2, // normal allocating alias. Don't use from ARM
    MEM_FLAG_DIRECT         = 1<<2, // 0xC alias uncached
    MEM_FLAG_COHERENT       = 2<<2, // 0x8 alias. Non-allocating in L2 but coherent
    MEM_FLAG_ZERO           = 1<<4, // initialise buffer to all zeros
    MEM_FLAG_NO_INIT        = 1<<5, // don't initialise (default is initialise to all ones)
    MEM_FLAG_HINT_PERMALOCK = 1<<6, // Likely to be locked for long periods of time
    MEM_FLAG_L1_NONALLOCATING=(MEM_FLAG_DIRECT | MEM_FLAG_COHERENT) // Allocating in L2
} VC_ALLOC_FLAGS;

// Size of memory page
#define PAGE_SIZE       0x1000
// Round up to nearest page
#define PAGE_ROUNDUP(n) ((n)%PAGE_SIZE==0 ? (n) : ((n)+PAGE_SIZE)&~(PAGE_SIZE-1))
#define FAIL(x) {printf(x); terminate(0);}

// Mailbox command/response structure
typedef struct {
    uint32_t len,   // Overall length (bytes)
        req,        // Zero for request, 1<<31 for response
        tag,        // Command number
        blen,       // Buffer length (bytes)
        dlen;       // Data length (bytes)
        uint32_t uints[32-5];   // Data (108 bytes maximum)
} VC_MSG __attribute__ ((aligned (16)));
  
// #define VIRT_GPIO_REG(a) ((uint32_t *)((uint32_t)virt_gpio_regs + (a)))
// #define GPIO_LEV0       0x34
// Open mailbox interface, return file descriptor

void terminate() {
    
}
int open_mbox(void)
{
   int fd;
 
   if ((fd = open("/dev/vcio", 0)) < 0)
       FAIL("Error: can't open VC mailbox\n");
   return(fd);
}
// Send message to mailbox, return first response int, 0 if error
uint32_t msg_mbox(int fd, VC_MSG *msgp)
{
    uint32_t ret=0, i;
 
    for (i=msgp->dlen/4; i<=msgp->blen/4; i+=4)
        msgp->uints[i++] = 0;
    msgp->len = (msgp->blen + 6) * 4;
    msgp->req = 0;
    if (ioctl(fd, _IOWR(100, 0, void *), msgp) < 0)
        printf("VC IOCTL failed\n");
    else if ((msgp->req&0x80000000) == 0)
        printf("VC IOCTL error\n");
    else if (msgp->req == 0x80000001)
        printf("VC IOCTL partial error\n");
    else
        ret = msgp->uints[0];
    return(ret);
}

// Allocate memory on PAGE_SIZE boundary, return handle
uint32_t alloc_vc_mem(int fd, uint32_t size, VC_ALLOC_FLAGS flags)
{
    VC_MSG msg={.tag=0x3000c, .blen=12, .dlen=12,
        .uints={PAGE_ROUNDUP(size), PAGE_SIZE, flags}};
    return(msg_mbox(fd, &msg));
}
// Lock allocated memory, return bus address
void *lock_vc_mem(int fd, int h)
{
    VC_MSG msg={.tag=0x3000d, .blen=4, .dlen=4, .uints={h}};
    return(h ? (void *)msg_mbox(fd, &msg) : 0);
}

// Get virtual memory segment for peripheral regs or physical mem
void *map_segment(void *addr, int size)
{
    int fd;
    void *mem;

    size = PAGE_ROUNDUP(size);
    if ((fd = open ("/dev/mem", O_RDWR|O_SYNC|O_CLOEXEC)) < 0)
        FAIL("Error: can't open /dev/mem, run using sudo\n");
    mem = mmap(0, size, PROT_WRITE|PROT_READ, MAP_SHARED, fd, (uint32_t)addr);
    close(fd);
#if DEBUG
    printf("Map %p -> %p\n", (void *)addr, mem);
#endif
    if (mem == MAP_FAILED)
        FAIL("Error: can't map memory\n");
    return(mem);
}

// Unlock allocated memory
uint32_t unlock_vc_mem(int fd, int h)
{
    VC_MSG msg={.tag=0x3000e, .blen=4, .dlen=4, .uints={h}};
    return(h ? msg_mbox(fd, &msg) : 0);
}

// Free memory
uint32_t free_vc_mem(int fd, int h)
{
    VC_MSG msg={.tag=0x3000f, .blen=4, .dlen=4, .uints={h}};
    return(h ? msg_mbox(fd, &msg) : 0);
}

uint32_t execute_code(int fd, uint32_t *addr, uint32_t arg1) {
    VC_MSG msg={.tag=0x30010, .blen=28, .dlen=28, .uints={(uint32_t)addr, arg1, 0, 0, 0, 0, 0}};
    return(msg_mbox(fd, &msg));
}

/* 000030fe <simple_rpmp>: */
uint8_t simple_rpmp[] = {
        0x10, 0x62,                           // add r0,0x1
        0x5a, 0x00,                           // rts
};
int simple_rpmp_len = 4;


void main() {
    int fd = open_mbox();
    printf("fd=%d\n", fd);
    int handle = alloc_vc_mem(fd, 0xffff, MEM_FLAG_DIRECT|MEM_FLAG_ZERO);
    printf("handle=%d\n", handle);
    void *addr = lock_vc_mem(fd, handle);
    void *vaddr = map_segment(addr, 0xffff);
    printf("addr=%x, vaddr=%x\n", addr, vaddr);
    memcpy(vaddr, simple_rpmp, simple_rpmp_len);
    int ret = execute_code(fd, addr, 1);
    printf("ret=%d\n", ret);
    unlock_vc_mem(fd, handle);
    free_vc_mem(fd, handle);
}