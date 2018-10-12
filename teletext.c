/*
   main.c - dispmanx teletext display
   Copyright 2015 Alistair Buxton <a.j.buxton@gmail.com>

   This file is part of raspi-teletext.

   raspi-teletext is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   raspi-teletext is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with raspi-teletext. If not, see <http://www.gnu.org/licenses/>.
*/


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>

#include "bcm_host.h"

#include "buffer.h"
#include "demo.h"

#ifndef ALIGN_UP
#define ALIGN_UP(x,y)  ((x + (y)-1) & ~((y)-1))
#endif

static DISPMANX_DISPLAY_HANDLE_T   display;
static DISPMANX_ELEMENT_HANDLE_T   element;
static DISPMANX_RESOURCE_HANDLE_T  resource[3];
static int next_resource = 0;

static unsigned short palette[256] = { 0x0, 0xffff, 0xf000 };

uint8_t *image;
VC_RECT_T image_rect;
#define TYPE (VC_IMAGE_8BPP)
#define WIDTH (370)
#define HEIGHT (32)
#define OFFSET (8)
#define PITCH (ALIGN_UP(WIDTH, 32))
#define ROW(n) (image+(PITCH*(n))+OFFSET)

uint16_t mask_even;
uint16_t mask_odd;

void vsync(DISPMANX_UPDATE_HANDLE_T u, void* arg)
{

    int ret;
    DISPMANX_UPDATE_HANDLE_T    update;

    update = vc_dispmanx_update_start( 10 );
    assert( update );
    ret = vc_dispmanx_element_change_source( update, element, resource[next_resource]);
    assert( ret == 0 );
    ret = vc_dispmanx_update_submit_sync( update );
    assert( ret == 0 );

    if(next_resource != 2) {

        int real_next_resource = next_resource ^ 1;
        next_resource = 2; // use filler if next callback called before this one ends

        // fill image
        int n;
        int m;
        if(real_next_resource == 0) {
            m = mask_even;
            for (n = 0; n < HEIGHT; n += 2) {
                if (!(m & 1)) get_packet(ROW(n) + 24); // +24 because clock never changes
                m >>= 1;
            }
        }
        else {
            m = mask_odd;
            for (n=0; n<HEIGHT; n+=2) {
                if (!(m&1)) get_packet(ROW(n+1)+24);
                m >>= 1;
            }
        }

        // write to resource
        ret = vc_dispmanx_resource_write_data(  resource[real_next_resource], TYPE, PITCH, image, &image_rect );
        assert( ret == 0 );

        next_resource = real_next_resource; // queue up next real resource

    }
}

int main(int argc, char *argv[])
{
    int ret;
    VC_RECT_T       src_rect;
    VC_RECT_T       dst_rect;

    DISPMANX_UPDATE_HANDLE_T    update;
    uint32_t                    vc_image_ptr;

    bcm_host_init();

    display = vc_dispmanx_display_open( 0 );

    image = calloc( 1, PITCH * HEIGHT ); // buffer 0
    assert(image);

    int c;
    char *mvalue = NULL;
    char *ovalue = NULL;
    while ((c = getopt(argc,argv,"m:o:")) != -1)
    {
        switch(c)
        {
            case 'm':
                mvalue = optarg;
                break;
            case 'o':
                ovalue = optarg;
                break;
        }
    }

    mask_even = 0; // default to all 16 vbi lines used on both fields
    mask_odd = 0;
    
    if (mvalue)
    {
        mask_even = strtol(mvalue,NULL,0);
        if (!ovalue)
            mask_odd = mask_even;
    }
    if (ovalue)
    {
        mask_odd = strtol(ovalue,NULL,0);
        if (!mvalue)
            mask_even = mask_odd;
    }
 
    // initialize image buffer with clock run in
    int n, m, clock = 0x275555;
    int even, odd;
    for (m=0; m<24; m++) {
        even = mask_even;
        odd = mask_odd;
        for (n=0; n<HEIGHT; n+=2) {
            if (!(even&1)) ROW(n)[m] = clock&1;
            if (!(odd&1)) ROW(n+1)[m] = clock&1;
            even >>= 1;
            odd >>= 1;
        }
        
        clock = clock >> 1;
    }
    
    // initialise active lines with filler packets
    even = mask_even;
    odd = mask_odd;
    for (n=0; n<HEIGHT; n+=2) {
        if (!(even&1)) get_packet(ROW(n)+24);
        if (!(odd&1)) get_packet(ROW(n+1)+24);
        even >>= 1;
        odd >>= 1;
    }

    // set up some resources
    vc_dispmanx_rect_set( &image_rect, 0, 0, WIDTH, HEIGHT);
    for (n=0;n<3;n++) {
        resource[n] = vc_dispmanx_resource_create( TYPE, WIDTH, HEIGHT, &vc_image_ptr );
        assert( resource[n] );
        ret = vc_dispmanx_resource_set_palette(  resource[n], palette, 0, sizeof palette );
        assert( ret == 0 );
        ret = vc_dispmanx_resource_write_data(  resource[n], TYPE, PITCH, image, &image_rect );
        assert( ret == 0 );
    }
    vc_dispmanx_rect_set( &image_rect, OFFSET+24, 0, 336, HEIGHT); // from now on, only copy the parts that change

    update = vc_dispmanx_update_start( 10 );
    assert( update );

    vc_dispmanx_rect_set( &src_rect, 0, 0, WIDTH << 16, HEIGHT << 16 );
    vc_dispmanx_rect_set( &dst_rect, 0, 0, 720, HEIGHT );
    element = vc_dispmanx_element_add( update, display, 2000,
                                       &dst_rect, resource[2], &src_rect,
                                       DISPMANX_PROTECTION_NONE,
                                       NULL, NULL, VC_IMAGE_ROT0 );

    ret = vc_dispmanx_update_submit_sync( update );
    assert( ret == 0 );

    // BUG: clear any existing callbacks, even to other apps.
    // https://github.com/raspberrypi/userland/issues/218
    vc_dispmanx_vsync_callback(display, NULL, NULL);

    vc_dispmanx_vsync_callback(display, vsync, NULL);

    if (argc >= 2 && strlen(argv[argc-1])==1 && argv[argc-1][0] == '-') { // last argument is a single '-'
        while(read_packets()) {
            ;
        }
    } else {
        demo();
    }

    vc_dispmanx_vsync_callback(display, NULL, NULL); // disable callback

    update = vc_dispmanx_update_start( 10 );
    assert( update );
    ret = vc_dispmanx_element_remove( update, element );
    assert( ret == 0 );
    ret = vc_dispmanx_update_submit_sync( update );
    assert( ret == 0 );
    for (n=0; n<3; n++) {
        ret = vc_dispmanx_resource_delete( resource[0] );
        assert( ret == 0 );
    }
    ret = vc_dispmanx_display_close( display );
    assert( ret == 0 );

    return 0;
}
