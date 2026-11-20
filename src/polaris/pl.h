/* Polaris boot manager - shared low-level definitions.
 *
 * Everything here compiles with Open Watcom wcc (16-bit, -3) both for the
 * stage-2 boot image (no C library at all) and for the DOS installer.
 */
#ifndef PL_H
#define PL_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;
typedef signed char    s8;
typedef short          s16;
typedef long           s32;

#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef __WATCOMC__
#define __far
#endif
#define FAR __far

#define MKFP(s, o) ((void FAR *)(((u32)(u16)(s) << 16) | (u16)(o)))
#define FPSEG(p)   ((u16)((u32)(void FAR *)(p) >> 16))
#define FPOFF(p)   ((u16)(u32)(void FAR *)(p))
#define LINEAR(p)  (((u32)FPSEG(p) << 4) + FPOFF(p))

/* Real-mode register image used by pl_intr(). */
typedef union {
    u32 x;
    u16 w;
    struct { u8 l, h; } b;
} reg32;

typedef struct {
    reg32 a, b, c, d, si, di;
    u16   ds, es, flags, bp;
} rmregs;

#define CF_FLAG 0x0001
#define ZF_FLAG 0x0040

/* --- assembly helpers (boot/bios.asm) ----------------------------------- */
void pl_intr(u8 intno, rmregs *r);
#pragma aux pl_intr "*" parm [ax] [si] modify [ax]

/* Jump to a boot sector at 0000:7C00.  Never returns. */
void pl_boot_jump(u8 drive, u16 si_offset, u16 entry_seg, u16 entry_off);
#pragma aux pl_boot_jump "*" parm [ax] [si] [bx] [cx] aborts

u8 pl_cpu_class(void);          /* 3 = 386, 4 = 486, 5 = CPUID capable */
#pragma aux pl_cpu_class "*" value [al] modify [ax bx cx dx]

/* EBX:EDX:ECX vendor string (12 bytes) and brand string (48 bytes) */
void pl_cpuid(u32 leaf, u32 *out4);
#pragma aux pl_cpuid "*" parm [dx ax] [si] modify [ax bx cx dx]

/* --- port I/O (inline) ---------------------------------------------------- */
u8 inb(u16 port);
#pragma aux inb = "in al,dx" parm [dx] value [al]
void outb(u16 port, u8 v);
#pragma aux outb = "out dx,al" parm [dx] [al]
u16 inw(u16 port);
#pragma aux inw = "in ax,dx" parm [dx] value [ax]
void outw(u16 port, u16 v);
#pragma aux outw = "out dx,ax" parm [dx] [ax]
u32 inl(u16 port);
#pragma aux inl = ".386" "in eax,dx" "mov edx,eax" "shr edx,16" parm [dx] value [dx ax]
void outl(u16 port, u32 v);
/* a long in [ax bx] arrives with AX = low word, BX = high word */
#pragma aux outl = ".386" "shl ebx,16" "mov bx,ax" "mov eax,ebx" "out dx,eax" parm [dx] [ax bx] modify [ax bx]
/* read cx words from port dx into es:di (es is always DGROUP here) */
void insw_buf(u16 port, void *buf, u16 words);
#pragma aux insw_buf = "push es" "push ds" "pop es" "cld" "rep insw" "pop es" \
        parm [dx] [di] [cx] modify [di cx]
void outsw_buf(u16 port, const void *buf, u16 words);
#pragma aux outsw_buf = "cld" "rep outsw" parm [dx] [si] [cx] modify [si cx]
void cli(void);
#pragma aux cli = "cli"
void sti(void);
#pragma aux sti = "sti"
void cpu_halt(void);
#pragma aux cpu_halt = "sti" "hlt"

/* --- tiny runtime (rt.c) -------------------------------------------------- */
void  pl_memset(void *d, u8 v, u16 n);
void  pl_memcpy(void *d, const void *s, u16 n);
int   pl_memcmp(const void *a, const void *b, u16 n);
void  pl_fmemcpy(void FAR *d, const void FAR *s, u16 n);
void  pl_fmemset(void FAR *d, u8 v, u16 n);
u16   pl_strlen(const char *s);
void  pl_strncpy(char *d, const char *s, u16 n);    /* always terminates */
char *pl_strcat(char *d, const char *s);
int   pl_strcmp(const char *a, const char *b);
char  pl_upper(char c);
/* number formatting helpers, all return d */
char *pl_utoa(char *d, u32 v);
char *pl_hex(char *d, u32 v, u8 digits);
char *pl_size(char *d, u32 sectors);   /* "1.2 GB" style from 512-byte sectors */
u16   pl_hash(const char *s);           /* password hash (never 0) */

/* BIOS helpers (rt.c) */
u32   pl_ticks(void);                   /* 18.2 Hz BIOS tick counter */
void  pl_delay(u16 ticks);
u16   pl_key_peek(void);                /* 0 if none waiting, otherwise key */
u16   pl_key_get(void);                 /* waits */
u8    pl_shift_state(void);

/* extended keys (scan << 8 | ascii) */
#define K_ESC    0x011B
#define K_ENTER  0x1C0D
#define K_KPENTER 0xE00D
#define K_TAB    0x0F09
#define K_STAB   0x0F00
#define K_BS     0x0E08
#define K_UP     0x4800
#define K_DOWN   0x5000
#define K_LEFT   0x4B00
#define K_RIGHT  0x4D00
#define K_PGUP   0x4900
#define K_PGDN   0x5100
#define K_HOME   0x4700
#define K_END    0x4F00
#define K_INS    0x5200
#define K_DEL    0x5300
#define K_F1     0x3B00
#define K_F2     0x3C00
#define K_F3     0x3D00
#define K_F4     0x3E00
#define K_F5     0x3F00
#define K_F6     0x4000
#define K_F7     0x4100
#define K_F8     0x4200
#define K_F9     0x4300
#define K_F10    0x4400
#define K_SPACE  0x3920

#define KEYSCAN(k) ((u8)((k) >> 8))
#define KEYCHAR(k) ((u8)(k))

#endif
