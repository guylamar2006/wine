/*
 * Copyright 2022 Jacek Caban for CodeWeavers
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "ntuser.h"
#include "wine/unixlib.h"

enum x11drv_funcs
{
    unix_clipboard_message,
    unix_create_desktop,
    unix_init,
    unix_tablet_attach_queue,
    unix_tablet_get_packet,
    unix_tablet_info,
    unix_tablet_load_info,
    unix_funcs_count,
};

/* FIXME: Use __wine_unix_call when the rest of the stack is ready */
extern NTSTATUS x11drv_unix_call( enum x11drv_funcs code, void *params ) DECLSPEC_HIDDEN;
#define X11DRV_CALL(func, params) x11drv_unix_call( unix_ ## func, params )

/* x11drv_clipboard_message params */
struct clipboard_message_params
{
    HWND hwnd;
    UINT msg;
    WPARAM wparam;
    LPARAM lparam;
};

/* x11drv_create_desktop params */
struct create_desktop_params
{
    UINT width;
    UINT height;
};

/* x11drv_tablet_info params */
struct tablet_info_params
{
    UINT category;
    UINT index;
    void *output;
};

/* DnD support */

struct format_entry
{
    UINT format;
    UINT size;
    char data[1];
};

enum dnd_event_type
{
    DND_DROP_EVENT,
    DND_LEAVE_EVENT,
    DND_POSITION_EVENT,
};

/* DND_DROP_EVENT params */
struct dnd_drop_event_params
{
    UINT type;
    HWND hwnd;
};

/* DND_POSITION_EVENT params */
struct dnd_position_event_params
{
    UINT  type;
    HWND  hwnd;
    POINT point;
    DWORD effect;
};
