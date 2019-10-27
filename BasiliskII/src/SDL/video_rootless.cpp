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
};

static uint32 rootless_proc_ptr = 0;

void InstallRootlessProc() {
    // Rootless mode support
    M68kRegisters r;
    if (PrefsFindBool("rootless")) {
        r.d[0] = sizeof(rootless_proc);
        Execute68kTrap(0xa71e, &r); // NewPtrSysClear()
        rootless_proc_ptr = r.a[0];
        printf("Installing rootless support: 0x%x\n", rootless_proc_ptr);
        Host2Mac_memcpy(rootless_proc_ptr, rootless_proc, sizeof(rootless_proc));
    } else {
        rootless_proc_ptr = 0;
    }
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

void update_display_mask(int w, int h) {
    if (rootless_proc_ptr == 0) {
        return;
    }
    
    uint32 expandMem = ReadMacInt32(0x02B6);
    uint16 emProcessMgrExists = ReadMacInt16(expandMem + 0x0128);
    if (!emProcessMgrExists) {
        return;
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
    
    // show menu bar
    uint16 menuBarHeight = ReadMacInt16(0x0BAA);
    MaskRect(0, 0, menuBarHeight, display_mask.w, true);
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
