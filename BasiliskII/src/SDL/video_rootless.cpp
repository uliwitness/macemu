//
//  video_rootless.cpp
//  BasiliskII
//
//  Created by Jesús A. Álvarez on 26/10/2019.
//

#include <SDL.h>
#include "sysdeps.h"
#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"

/*
 *  Rootless mode support
 */

static const uint8 rootless_proc[] = {
    // PEntryFromProcessSerialNumber(GetNextProcess(d0:d1))
    // returns next PSN in d0:d1, PEntryPtr in d2
    0x2F, 0x01,             // move.l d1,-(sp)     ; psn low
    0x2F, 0x00,             // move.l d0,-(sp)     ; psn high
    0x55, 0x4F,             // suba.w #2,sp        ; result
    0x48, 0x6F, 0x00, 0x02, // pea 2(sp)           ; ptr(psn)
    0x3F, 0x3C, 0x00, 0x38, // move.w #$38,-(sp)   ; GetNextProcess
    0xA8, 0x8F,             // _OSDispatch         ; get psn of first process
    0x30, 0x1F,             // move.w (sp)+,d0     ; result

    0x59, 0x4F,             // subq #4,sp          ; result
    0x48, 0x6F, 0x00, 0x04, // pea 4(sp)           ; ptr(psn)
    0x3F, 0x3C, 0x00, 0x4F, // move.w #$4F,-(sp)   ; _PEntryFromProcessSerialNumber
    0xA8, 0x8F,             // _OSDispatch         ; get PEntry
    0x24, 0x1F,             // move.l (sp)+, d2    ; pEntry
    0x20, 0x1F,             // move.l (sp)+, d0    ; psn high
    0x22, 0x1F,             // move.l (sp)+, d1    ; psn low
    0x4E, 0x75,             // rts
    
    // a0 = GetResource(d0, d1)
#define GETRESOURCE 38      // offset to this function
    0x59, 0x4F,             // subq #4,sp          ; result
    0x2F, 0x00,             // move.l d0,-(sp)     ; type
    0x3F, 0x01,             // move.w d1,-(sp)     ; size
    0xA9, 0xA0,             // _GetResource
    0x20, 0x57,             // movea.l (sp),a0     ; result to a0
    0x59, 0x4F,             // subq #4, sp         ; result
    0x2F, 0x08,             // move.l a0,-(sp)     ; handle
    0xA9, 0xA5,             // _SizeRsrc
    0x20, 0x1F,             // move.l (sp)+,d0     ; size
    0x20, 0x5F,             // movea.l (sp)+,a0    ; handle
    0x4E, 0x75,             // rts
};

static uint32 rootless_proc_ptr = 0;
static uint32 low_mem_map = 0;

int16 InstallRootlessProc() {
    // Rootless mode support
    M68kRegisters r;
    if (PrefsFindBool("rootless")) {
        printf("Installing rootless support\n");
        r.d[0] = sizeof(rootless_proc);
        Execute68kTrap(0xa71e, &r); // NewPtrSysClear()
        if (r.a[0] == 0) {
            return memFullErr;
        }
        rootless_proc_ptr = r.a[0];
        Host2Mac_memcpy(rootless_proc_ptr, rootless_proc, sizeof(rootless_proc));
        low_mem_map = 0;
        printf("Installed at 0x%x\n", rootless_proc_ptr);
    } else {
        rootless_proc_ptr = 0;
        low_mem_map = 0;
    }
    return noErr;
}

static struct {
    uint8_t *pixels;
    int w,h;
} display_mask = {
    .pixels = NULL,
    .w = 0,
    .h = 0
};

void MaskRect(int16 top, int16 left, int16 bottom, int16 right, bool in) {
    if (top == bottom || left == right) return;
    if (top < 0) top = 0;
    if (left < 0) left = 0;
    
    uint8_t *line = display_mask.pixels + display_mask.w * top + left;
    for (int y = top; y < bottom; y++) {
        memset(line, in ? 0xff : 0x00, right - left);
        line += display_mask.w;
    }
}

void PrintRegion(uint32 regionPtr) {
    int16 size = ReadMacInt16(regionPtr);
    int16 top = ReadMacInt16(regionPtr + 2);
    int16 left = ReadMacInt16(regionPtr + 4);
    int16 bottom = ReadMacInt16(regionPtr + 6);
    int16 right = ReadMacInt16(regionPtr + 8);
    printf("Region (%d: %d,%d %dx%d):\n", size, left, top, (right-left), (bottom-top));
    for(int i=0; i < size; i++) {
        printf("%02x", ReadMacInt8(regionPtr + i));
    }
    printf("\n");
}

void MaskRegion(uint32 regionPtr, bool in) {
    // https://www.info-mac.org/viewtopic.php?t=17328
    uint16 size = ReadMacInt16(regionPtr);
    int16 top = ReadMacInt16(regionPtr + 2);
    int16 left = ReadMacInt16(regionPtr + 4);
    int16 bottom = ReadMacInt16(regionPtr + 6);
    int16 right = ReadMacInt16(regionPtr + 8);
    
    if (size == 10) {
        MaskRect(top, left, bottom, right, in);
        return;
    }
    
    uint8_t *scanline = display_mask.pixels + top * display_mask.w;
    uint8_t *curLine = (uint8*)alloca(display_mask.w);
    memset(curLine, 0, display_mask.w);
    
    uint32 ptr = regionPtr + 10;
    for (int16 y = top; y < bottom; y++) {
        uint16 nextLine = ReadMacInt16(ptr);
        if (nextLine == y) {
            // apply changes to current line
            ptr += 2;
            for(;;) {
                uint16 begin = ReadMacInt16(ptr);
                ptr += 2;
                if (begin == 0x7fff) break;
                uint16 end = ReadMacInt16(ptr);
                ptr += 2;
                for (int i=begin; i < end; i++) {
                    curLine[i] ^= 1;
                }
            }
        }
        
        // blit current line
        if (in) {
            for (int x = left; x < right; x++) {
                scanline[x] |= curLine[x];
            }
        } else {
            for (int x = left; x < right; x++) {
                scanline[x] |= !curLine[x];
            }
        }
        
        scanline += display_mask.w;
    }
}

uint32 GetLowMemOffset(uint32 addr) {
    if (low_mem_map == 0) {
        abort();
    }
    
    uint32 offset = 0;
    uint32 ptr = low_mem_map;
    for(;;) {
        uint16 size = ReadMacInt16(ptr);
        if (size == 0) break;
        uint32 lo = ReadMacInt32(ptr+2);
        ptr += 6;
        if (addr < lo) {
            return UINT32_MAX;
        } else if (addr < (lo+size)) {
            return offset + (addr-lo);
        }
        offset += size;
    }
    return UINT32_MAX;
}

static uint32 GetResource(uint32 type, int16 id, int32* size) {
    M68kRegisters r;
    r.d[0] = type;
    r.d[1] = id;
    Execute68k(rootless_proc_ptr + GETRESOURCE, &r);
    if (size) {
        *size = r.d[0];
    }
    return r.a[0];
}

static void MaskMenuBar() {
    uint16 menuBarHeight = ReadMacInt16(0x0BAA);
    MaskRect(0, 0, menuBarHeight, display_mask.w, true);
}

static void MaskMenu(uint32 mbEntry) {
    int16 menuTop = ReadMacInt16(mbEntry);
    int16 menuLeft = ReadMacInt16(mbEntry + 2);
    int16 menuBottom = ReadMacInt16(mbEntry + 4);
    int16 menuRight = ReadMacInt16(mbEntry + 6);
    MaskRect(menuTop, menuLeft-1, menuBottom+1, menuRight+1, true);
    // shadow
    MaskRect(menuBottom+1, menuLeft+1, menuBottom+2, menuRight+1, true);
    MaskRect(menuTop+2, menuRight+1, menuBottom+2, menuRight+2, true);
}

uint16 menuEntries[16];
uint16 *lastMenuEntry = menuEntries;

static void MaskMenus(uint32 expandMem, uint32 lowMemPtr) {
    uint32 emHelpGlobals = ReadMacInt32(expandMem + 0x78);
    uint16 inMenuSelect = ReadMacInt16(emHelpGlobals + 0x11c);
    if (!inMenuSelect) {
        // clean up
        lastMenuEntry = menuEntries;
        *lastMenuEntry = 0;
        return;
    }
    
    uint32 mbSaveLoc = ReadMacInt32(ReadMacInt32(lowMemPtr + GetLowMemOffset(0x0B5C)));
    if (mbSaveLoc == 0) {
        // no menu yet
        return;
    }
    
    uint16 mbEntryOffset = ReadMacInt16(mbSaveLoc);
    if (lastMenuEntry == menuEntries && *lastMenuEntry == 0) {
        // first menu
        *lastMenuEntry = mbEntryOffset;
    } else if (mbEntryOffset > *lastMenuEntry) {
        // added menu
        *(++lastMenuEntry) = mbEntryOffset;
    } else if (mbEntryOffset < *lastMenuEntry) {
        // removed menu
        lastMenuEntry--;
    }
    
    // mask all menus
    for (uint16 *entry = menuEntries; entry <= lastMenuEntry; entry++) {
        MaskMenu(mbSaveLoc + *entry);
    }
}

void update_display_mask(int w, int h) {
    if (rootless_proc_ptr == 0) {
        return;
    }
    
    // Check for process manager
    uint32 expandMem = ReadMacInt32(0x02B6);
    uint16 emProcessMgrExists = ReadMacInt16(expandMem + 0x0128);
    if (!emProcessMgrExists) {
        return;
    }
    
    // Read lowmem mapping
    if (low_mem_map == 0) {
        uint32 handle = GetResource('lmem', -16458, NULL);
        low_mem_map = ReadMacInt32(handle);
    }
    
    if (display_mask.w != w || display_mask.h != h) {
        // new mask
        free(display_mask.pixels);
        display_mask.pixels = (uint8_t*)calloc(1, w*h);
        display_mask.w = w;
        display_mask.h = h;
    }
    
    // clear all
    memset(display_mask.pixels, 0, display_mask.w * display_mask.h);
    
    // hide desktop
    uint32 deskPort = ReadMacInt32(0x9E2);
    uint32 deskPortVisRgn = ReadMacInt32(ReadMacInt32(deskPort + 0x18));
    MaskRegion(deskPortVisRgn, false);
    
    MaskMenuBar();
    
    M68kRegisters r;
    // Find frontmost process
    for(r.d[0] = 0, r.d[1] = 0;;) {
        Execute68k(rootless_proc_ptr, &r);
        uint32_t pEntryPtr = r.d[2];
        if (r.d[2] == 0) break;
        
        uint16 state = ReadMacInt16(pEntryPtr);
        if (state != 4) break;
        
        uint32 lowMemPtr = ReadMacInt32(ReadMacInt32(pEntryPtr + 0x9E));
        MaskMenus(expandMem, lowMemPtr);
    }
}

void apply_display_mask(SDL_Surface * host_surface, SDL_Rect update_rect) {
    if (display_mask.pixels == NULL) {
        return;
    }
    
    printf("apply video mask (%d,%d->%dx%d)\n", (int)update_rect.x, (int)update_rect.y, (int)update_rect.w, (int)update_rect.h);
    
    if (host_surface->format->format != SDL_PIXELFORMAT_ARGB8888) {
        printf("Invalid host surface\n");
        return;
    }
    
    uint32_t * srcPixels = (uint32_t*)((uint8_t *)host_surface->pixels +
                                       update_rect.y * host_surface->pitch +
                                       update_rect.x * 4);
    
    uint8_t * srcMask = display_mask.pixels + update_rect.y * display_mask.w + update_rect.x;
    for (int y = update_rect.y; y < update_rect.y+update_rect.h; y++) {
        uint32_t * pixel = srcPixels;
        uint8_t * mask = srcMask;
        for (int x = update_rect.x; x < update_rect.x+update_rect.w; x++) {
            if (*mask == 0) {
                *pixel = 0;
            }
            pixel++;
            mask++;
        }
        srcPixels += host_surface->pitch / 4;
        srcMask += display_mask.w;
    }
}
