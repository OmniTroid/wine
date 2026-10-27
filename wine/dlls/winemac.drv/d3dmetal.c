/*
 * Mac graphics driver hooks used by D3DMetal (part of the Apple Game Porting Toolkit)
 *
 * Copyright 2023 Brendan Shanks for CodeWeavers, Inc.
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

#if 0
#pragma makedep unix
#endif

#if defined(__x86_64__)

#include "config.h"

#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "macdrv.h"
#include "shellapi.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(macdrv_d3dmtl);

typedef LONG LSTATUS;

struct macdrv_functions_t
{
    void (*macdrv_init_display_devices)(BOOL);
    struct d3dmetal_macdrv_win_data* (*get_win_data)(HWND hwnd);
    void (*release_win_data)(struct d3dmetal_macdrv_win_data *data);
    macdrv_window(*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
    macdrv_metal_device (*macdrv_create_metal_device)(void);
    void (*macdrv_release_metal_device)(macdrv_metal_device d);
    macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
    macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
    void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
    void (*on_main_thread)(dispatch_block_t b);
    LSTATUS(WINAPI*RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, BYTE*, LPDWORD);
    LSTATUS(WINAPI*RegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
    LSTATUS(WINAPI*RegOpenKeyExA)(HKEY, LPCSTR, DWORD, DWORD, HKEY*);
    LSTATUS(WINAPI*RegCreateKeyExA)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, HKEY*, LPDWORD);
    LSTATUS(WINAPI*RegCloseKey)(HKEY);
    BOOL(WINAPI*EnumDisplayMonitors)(HDC,LPRECT,MONITORENUMPROC,LPARAM);
    BOOL(WINAPI*GetMonitorInfoA)(HMONITOR,LPMONITORINFO);
    BOOL(WINAPI*AdjustWindowRectEx)(LPRECT,DWORD,BOOL,DWORD);
    LONG_PTR(WINAPI*GetWindowLongPtrW)(HWND,int);
    BOOL(WINAPI*GetWindowRect)(HWND,LPRECT);
    BOOL(WINAPI*MoveWindow)(HWND,int,int,int,int,BOOL);
    BOOL(WINAPI*SetWindowPos)(HWND,HWND,int,int,int,int,UINT);
    INT(WINAPI*GetSystemMetrics)(INT);
    LONG_PTR(WINAPI*SetWindowLongPtrW)(HWND,INT,LONG_PTR);
};
C_ASSERT(sizeof(struct macdrv_functions_t) == 192);

/* macdrv private window data expected by D3DMetal */
struct d3dmetal_macdrv_win_data
{
    HWND                hwnd;                   /* hwnd that this private data belongs to */
    macdrv_window       cocoa_window;
    macdrv_view         cocoa_view;
    macdrv_view         client_cocoa_view;
    RECT                window_rect;            /* USER window rectangle relative to parent */
    RECT                whole_rect;             /* Mac window rectangle for the whole window relative to parent */
    RECT                client_rect;            /* client area relative to parent */
    int                 pixel_format;           /* pixel format for GL */
    COLORREF            color_key;              /* color key for layered window; CLR_INVALID is not color keyed */
    HANDLE              drag_event;             /* event to signal that Cocoa-driven window dragging has ended */
    unsigned int        on_screen : 1;          /* is window ordered in? (minimized or not) */
    unsigned int        shaped : 1;             /* is window using a custom region shape? */
    unsigned int        layered : 1;            /* is window layered and with valid attributes? */
    unsigned int        ulw_layered : 1;        /* has UpdateLayeredWindow() been called for window? */
    unsigned int        per_pixel_alpha : 1;    /* is window using per-pixel alpha? */
    unsigned int        minimized : 1;          /* is window minimized? */
    void *              padding[2];             /* used to be struct window_surface* surface/unminimized_surface */
};

C_ASSERT(sizeof(struct d3dmetal_macdrv_win_data) == 120);

void OnMainThread(dispatch_block_t block);
static void cf_client_surface_release(CFAllocatorRef allocator, const void *client_surface);

/* Highball: cross-process swapchains (Wine bug 60263). A window whose root belongs to another
 * process (a CEF GPU process presenting into the browser process's window: Steam, Rockstar,
 * Ubisoft Connect) has no Cocoa view of ours to draw into. Its client surface renders into a
 * CAContext-backed layer that the root's owner hosts, see
 * macdrv_client_surface_acquire_metal_swapchain(). D3DMetal/DXMT are none the wiser: the
 * surface's view is handed out as client_cocoa_view, doubles as the "metal view" handle, and the
 * layer behind it is the offscreen swapchain's. Surfaces are keyed by that view, not by HWND: the
 * same HWND is asked for a swapchain several times (CEF recreates it on every resize). */
static pthread_mutex_t remote_layer_mutex = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef remote_layer_surfaces;   /* view -> client_surface D3DMetal/DXMT still uses */

/* One retired surface held per child HWND. CAContextSwapChain's dealloc un-hosts the layer, and
 * a client that resizes by destroying its swapchain and creating a new one would have nothing
 * hosted between the two; so a released surface is held until the child's next release, which
 * follows its successor's creation (the owner retires the old layer when the new one is hosted).
 * A destroyed child never releases again, so a release whose child is gone is not held, and
 * every release sweeps out dead slots. */
static CFMutableDictionaryRef pending_by_child;        /* child HWND -> retired client_surface */

static struct client_surface *remote_layer_surface_for_view(macdrv_view view)
{
    struct client_surface *client = NULL;

    if (!view) return NULL;
    pthread_mutex_lock(&remote_layer_mutex);
    if (remote_layer_surfaces)
        client = (struct client_surface *)CFDictionaryGetValue(remote_layer_surfaces, view);
    pthread_mutex_unlock(&remote_layer_mutex);
    return client;
}

static BOOL remote_layer_surface_add(struct macdrv_client_surface *surface)
{
    BOOL ret;

    pthread_mutex_lock(&remote_layer_mutex);
    if (!remote_layer_surfaces)
        remote_layer_surfaces = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    if ((ret = remote_layer_surfaces && surface->cocoa_view))
        CFDictionarySetValue(remote_layer_surfaces, surface->cocoa_view, &surface->client);
    pthread_mutex_unlock(&remote_layer_mutex);
    return ret;
}

/* The surface behind a view, removed from the table; NULL when the view is not one of ours. */
static struct client_surface *remote_layer_surface_take(macdrv_view view)
{
    struct client_surface *client = NULL;

    if (!view) return NULL;
    pthread_mutex_lock(&remote_layer_mutex);
    if (remote_layer_surfaces &&
        (client = (struct client_surface *)CFDictionaryGetValue(remote_layer_surfaces, view)))
        CFDictionaryRemoveValue(remote_layer_surfaces, view);
    pthread_mutex_unlock(&remote_layer_mutex);
    return client;
}

/* Remove every held surface whose child window no longer exists, for release outside the lock.
 * Called with remote_layer_mutex held; at most max per call, the rest on the next release. */
static CFIndex collect_dead_pending(struct client_surface **out, CFIndex max)
{
    CFIndex count, i, n = 0;
    const void **keys, **vals;

    if (!pending_by_child || !(count = CFDictionaryGetCount(pending_by_child))) return 0;
    keys = calloc(count, sizeof(*keys));
    vals = calloc(count, sizeof(*vals));
    if (!keys || !vals) { free(keys); free(vals); return 0; }
    CFDictionaryGetKeysAndValues(pending_by_child, keys, vals);
    for (i = 0; i < count && n < max; i++)
    {
        if (NtUserIsWindow((HWND)keys[i])) continue;
        out[n++] = (struct client_surface *)vals[i];
        CFDictionaryRemoveValue(pending_by_child, keys[i]);
    }
    free(keys);
    free(vals);
    return n;
}

/* Retire the surface behind a view D3DMetal/DXMT is finished with; TRUE if the view was one of
 * ours, so the caller falls through to the ordinary metal view release when it was not. */
static BOOL release_remote_layer_view(macdrv_view view)
{
    struct client_surface *client, *drain = NULL;
    struct client_surface *dead[16];
    CFIndex ndead, i;
    HWND owner;

    if (!(client = remote_layer_surface_take(view))) return FALSE;

    owner = client->hwnd;
    pthread_mutex_lock(&remote_layer_mutex);
    if (!pending_by_child)
        pending_by_child = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    if (!pending_by_child || !owner || !NtUserIsWindow(owner))
    {
        /* no table, or the child is already gone and no successor will ever come: release now */
        drain = client;
    }
    else
    {
        drain = (struct client_surface *)CFDictionaryGetValue(pending_by_child, owner);
        CFDictionarySetValue(pending_by_child, owner, client);
    }
    ndead = collect_dead_pending(dead, ARRAY_SIZE(dead));
    pthread_mutex_unlock(&remote_layer_mutex);

    TRACE("view %p child %p: %s, draining %p and %ld dead-child slot(s)\n", view, owner,
          drain == client ? "child gone, released now" : "held", drain, (long)ndead);
    if (drain) client_surface_release(drain);
    for (i = 0; i < ndead; i++) client_surface_release(dead[i]);
    return TRUE;
}

static void my_macdrv_init_display_devices(BOOL p1)
{
    TRACE("macdrv_init_display_devices %d - no-op\n", p1);
}

static struct d3dmetal_macdrv_win_data *my_get_win_data(HWND hwnd)
{
    struct macdrv_win_data *data;
    struct d3dmetal_macdrv_win_data *d3dm_data;
    struct macdrv_client_surface *client_surface;
    BOOL remote;
    TRACE("get_win_data %p\n", hwnd);

    /* Creating a client surface on each call to get_win_data() means it's no longer idempotent,
     * but D3DMetal/DXMT both call it only when creating a new DXGI swapchain.
     * They do:
     * get_win_data() -> create_metal_device() -> create_metal_view() -> get_metal_layer() -> release_win_data()
     */
    client_surface = macdrv_client_surface_create(hwnd);

    /* Highball: a window whose root another process owns gets a remote layer instead of a view
     * in a window of ours; the layer is created once the renderer hands over its Metal device,
     * in my_macdrv_view_create_metal_view(). */
    remote = macdrv_window_has_foreign_root(hwnd, NULL);

    /* get_win_data() needs to happen after client_surface creation to avoid deadlocks */
    data = get_win_data(hwnd);
    if (!data && !remote)
    {
        client_surface_release((struct client_surface *)client_surface);
        return NULL;
    }

    macdrv_set_view_d3dmetal_client_surface(client_surface->cocoa_view, &client_surface->client);

    if (remote)
    {
        /* owned by the tables above, not by the window's data: the window may not have any */
        if (!remote_layer_surface_add(client_surface))
        {
            release_win_data(data);
            client_surface_release((struct client_surface *)client_surface);
            return NULL;
        }
        TRACE("hwnd %p: root belongs to another process, surface %p view %p\n", hwnd,
              client_surface, client_surface->cocoa_view);
    }
    else
    {
        if (!data->d3dmetal_client_surfaces)
        {
            static const CFArrayCallBacks callbacks = { .release = cf_client_surface_release };
            data->d3dmetal_client_surfaces = CFArrayCreateMutable(NULL, 0, &callbacks);
        }
        CFArrayAppendValue(data->d3dmetal_client_surfaces, client_surface);
    }

    d3dm_data = calloc(1, sizeof(*d3dm_data));

    d3dm_data->hwnd = hwnd;
    /* cocoa_view is no longer present in macdrv_win_data. D3DMetal doesn't use it. */
    d3dm_data->client_cocoa_view = client_surface->cocoa_view;
    if (data)
    {
        d3dm_data->cocoa_window = data->cocoa_window;
        d3dm_data->window_rect = data->rects.window;
        d3dm_data->whole_rect = data->rects.visible;
        d3dm_data->client_rect = data->rects.client;
        d3dm_data->pixel_format = data->pixel_format;
        /* color_key is no longer present in macdrv_win_data. Assume D3DMetal doesn't use it. */
        d3dm_data->drag_event = data->drag_event;
        d3dm_data->on_screen = data->on_screen;
        d3dm_data->shaped = data->shaped;
        d3dm_data->layered = data->layered;
        d3dm_data->ulw_layered = data->ulw_layered;
        d3dm_data->per_pixel_alpha = data->per_pixel_alpha;
        d3dm_data->minimized = data->minimized;
        /* swap_interval is no longer present in macdrv_win_data. Assume D3DMetal doesn't use it. */
        /* surface/unminimized_surface are no longer present in macdrv_win_data. Assume D3DMetal doesn't use it. */
    }
    else
    {
        /* Highball: a window this process never positioned, owned by another process. The rects
         * are the ones the driver would have recorded, relative to the parent's client area. */
        HWND parent = NtUserGetAncestor(hwnd, GA_PARENT);
        UINT dpi = NtUserGetWinMonitorDpi(hwnd, MDT_RAW_DPI);
        RECT window_rect, client_rect;

        if (NtUserGetWindowRect(hwnd, &window_rect, dpi))
        {
            if (parent) NtUserMapWindowPoints(0, parent, (POINT *)&window_rect, 2, dpi);
            d3dm_data->window_rect = d3dm_data->whole_rect = window_rect;
        }
        if (NtUserGetClientRect(hwnd, &client_rect, dpi))
        {
            NtUserMapWindowPoints(hwnd, parent, (POINT *)&client_rect, 2, dpi);
            d3dm_data->client_rect = client_rect;
        }
        d3dm_data->on_screen = !!(NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE);
    }

    if (remote)
    {
        /* Highball: nothing here needs the window's data locked while the renderer sets up, and
         * the swapchain creation below waits on the main thread. */
        release_win_data(data);
        d3dm_data->padding[0] = NULL;
    }
    else
        d3dm_data->padding[0] = data;

    return d3dm_data;
}

static void my_release_win_data(struct d3dmetal_macdrv_win_data *data)
{
    TRACE("release_win_data %p\n", data);

    if (!data)
        return;

    if (data->padding[0]) release_win_data(data->padding[0]);
    free(data);
}

static macdrv_window my_macdrv_get_cocoa_window(HWND hwnd, BOOL require_on_screen)
{
    TRACE("macdrv_get_cocoa_window %p %d\n", hwnd, require_on_screen);
    return macdrv_get_cocoa_window(hwnd, require_on_screen);
}

static macdrv_metal_device my_macdrv_create_metal_device(void)
{
    TRACE("macdrv_create_metal_device\n");
    return macdrv_create_metal_device();
}

static void my_macdrv_release_metal_device(macdrv_metal_device d)
{
    TRACE("macdrv_release_metal_device %p\n", d);
    macdrv_release_metal_device(d);
}

static macdrv_metal_view my_macdrv_view_create_metal_view(macdrv_view v, macdrv_metal_device d)
{
    struct client_surface *client;

    TRACE("macdrv_view_create_metal_view %p %p\n", v, d);

    /* Highball: a remote surface's view is a placeholder. The layer it hands out is the offscreen
     * swapchain's, created here with the renderer's device, and the view itself is the handle
     * the renderer gets back, so that get_metal_layer() and release_metal_view() find it. */
    if ((client = remote_layer_surface_for_view(v)))
    {
        struct macdrv_client_surface *surface = impl_from_client_surface(client);

        if (!macdrv_client_surface_acquire_metal_swapchain(surface, d))
        {
            ERR("no remote swapchain for %s\n", debugstr_client_surface(client));
            if (remote_layer_surface_take(v)) client_surface_release(client);
            return NULL;
        }
        return (macdrv_metal_view)v;
    }

    return macdrv_view_create_metal_view(v, d);
}

static macdrv_metal_layer my_macdrv_view_get_metal_layer(macdrv_metal_view v)
{
    struct client_surface *client;

    TRACE("macdrv_view_get_metal_layer %p\n", v);

    if ((client = remote_layer_surface_for_view((macdrv_view)v)))
        return macdrv_swapchain_get_layer(impl_from_client_surface(client)->metal_swapchain);

    return macdrv_view_get_metal_layer(v);
}

static void my_macdrv_view_release_metal_view(macdrv_metal_view v)
{
    TRACE("macdrv_view_release_metal_view %p\n", v);

    if (release_remote_layer_view((macdrv_view)v)) return;

    return macdrv_view_release_metal_view(v);
}

static void my_OnMainThread(dispatch_block_t b)
{
    TRACE("OnMainThread %p\n", b);
    OnMainThread(b);
}


static LSTATUS WINAPI my_RegQueryValueExA(HKEY p1, LPCSTR p2, LPDWORD p3, LPDWORD p4, BYTE* p5, LPDWORD p6)
{
    LSTATUS result;
    void *ret_ptr;
    ULONG ret_len;
    struct regqueryvalueexa_params params =
    {
        .dispatch = {.callback = regqueryvalueexa_callback},
        .hkey = HandleToUlong(p1),
        .name = (UINT_PTR)p2,
        .reserved = (UINT_PTR)p3,
        .type = (UINT_PTR)p4,
        .data = (UINT_PTR)p5,
        .count = (UINT_PTR)p6,
        .result = (UINT_PTR)&result,
    };

    TRACE("RegQueryValueExA %p %s %p %p %p %p\n", p1, p2, p3, p4, p5, p6);

    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr, &ret_len);

    return result;
}

static LSTATUS WINAPI my_RegSetValueExA(HKEY p1, LPCSTR p2, DWORD p3, DWORD p4, const BYTE* p5, DWORD p6)
{
    LSTATUS result;
    void *ret_ptr;
    ULONG ret_len;
    struct regsetvalueexa_params params =
    {
        .dispatch = {.callback = regsetvalueexa_callback},
        .hkey = HandleToUlong(p1),
        .name = (UINT_PTR)p2,
        .reserved = p3,
        .type = p4,
        .data = (UINT_PTR)p5,
        .count = p6,
        .result = (UINT_PTR)&result,
    };

    TRACE("RegSetValueExA %p %s(%p) %d %p %d\n", p1, p2, p2, p4, p5, p6);

    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr, &ret_len);

    return result;
}

static LSTATUS WINAPI my_RegOpenKeyExA(HKEY p1, LPCSTR p2, DWORD p3, DWORD p4, HKEY* p5)
{
    LSTATUS result;
    void *ret_ptr;
    ULONG ret_len;
    struct regcreateopenkeyexa_params params =
    {
        .dispatch = {.callback = regcreateopenkeyexa_callback},
        .create = 0,
        .hkey = HandleToUlong(p1),
        .name = (UINT_PTR)p2,
        .options = p3,
        .access = p4,
        .retkey = (UINT_PTR)p5,
        .result = (UINT_PTR)&result,
    };

    TRACE("RegOpenKeyExA %p %s\n", p1, p2);

    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr, &ret_len);

    return result;
}

static LSTATUS WINAPI my_RegCreateKeyExA(HKEY p1, LPCSTR p2, DWORD p3, LPSTR p4, DWORD p5, DWORD p6, LPSECURITY_ATTRIBUTES p7, HKEY* p8, LPDWORD p9)
{
    LSTATUS result;
    void *ret_ptr;
    ULONG ret_len;
    struct regcreateopenkeyexa_params params =
    {
        .dispatch = {.callback = regcreateopenkeyexa_callback},
        .create = 1,
        .hkey = HandleToUlong(p1),
        .name = (UINT_PTR)p2,
        .reserved = p3,
        .class = (UINT_PTR)p4,
        .options = p5,
        .access = p6,
        .security = (UINT_PTR)p7,
        .retkey = (UINT_PTR)p8,
        .disposition = (UINT_PTR)p9,
        .result = (UINT_PTR)&result,
    };

    TRACE("RegCreateKeyExA %p %s\n", p1, p2);

    KeUserDispatchCallback(&params.dispatch, sizeof(params), &ret_ptr, &ret_len);

    return result;
}

static LSTATUS WINAPI DECLSPEC_HOTPATCH RegCloseKey( HKEY hkey )
{
    if (!hkey) return ERROR_INVALID_HANDLE;
    if (hkey >= (HKEY)0x80000000) return ERROR_SUCCESS;
    return RtlNtStatusToDosError( NtClose( hkey ) );
}

static LSTATUS WINAPI my_RegCloseKey(HKEY hkey)
{
    TRACE("RegCloseKey %p\n", hkey);
    return RegCloseKey(hkey);
}

static BOOL WINAPI my_EnumDisplayMonitors(HDC h, LPRECT p2, MONITORENUMPROC p3, LPARAM p4)
{
    TRACE("EnumDisplayMonitors %p %p %p %ld\n", h, p2, p3, p4);
    return NtUserEnumDisplayMonitors(h, p2, p3, p4);
}

static BOOL WINAPI my_GetMonitorInfoA(HMONITOR monitor, LPMONITORINFO info)
{
    MONITORINFOEXW miW;
    BOOL ret;

    TRACE("GetMonitorInfoA %p %p\n", monitor, info);

    if (info->cbSize == sizeof(MONITORINFO)) return NtUserGetMonitorInfo( monitor, info );
    if (info->cbSize != sizeof(MONITORINFOEXA)) return FALSE;

    miW.cbSize = sizeof(miW);
    ret = NtUserGetMonitorInfo( monitor, (MONITORINFO *)&miW );
    if (ret)
    {
        MONITORINFOEXA *miA = (MONITORINFOEXA *)info;
        ULONG size;
        miA->rcMonitor = miW.rcMonitor;
        miA->rcWork = miW.rcWork;
        miA->dwFlags = miW.dwFlags;
        RtlUnicodeToUTF8N(miA->szDevice, sizeof(miA->szDevice), &size, miW.szDevice, lstrlenW(miW.szDevice) * sizeof(WCHAR));
    }
    return ret;
}

static BOOL WINAPI my_AdjustWindowRectEx(LPRECT p1,DWORD p2,BOOL p3,DWORD p4)
{
    TRACE("AdjustWindowRectEx %p %u %d %u\n", p1, p2, p3, p4);
    return NtUserAdjustWindowRect(p1, p2, p3, p4, NtUserGetSystemDpiForProcess(NULL));
}

static LONG_PTR WINAPI my_GetWindowLongPtrW(HWND h,int nIndex)
{
    TRACE("GetWindowLongPtrW %p\n", h);
    /* ignore possibility of DWLP_DLGPROC */
    return NtUserGetWindowLongPtrW(h, nIndex);
}

static BOOL WINAPI my_GetWindowRect(HWND h, LPRECT rect)
{
    TRACE("GetWindowRect %p %p\n", h, rect);
    return NtUserGetWindowRect(h, rect, NtUserGetWinMonitorDpi(h, MDT_DEFAULT));
}

static BOOL WINAPI my_MoveWindow(HWND h, int X,int Y,int nWidth,int nHeight,BOOL bRepaint)
{
    TRACE("MoveWindow %p %d %d %d %d %d\n", h, X, Y, nWidth, nHeight, bRepaint);
    return NtUserMoveWindow(h, X, Y, nWidth, nHeight, bRepaint);
}

static BOOL WINAPI my_SetWindowPos(HWND h,HWND h2,int x,int y,int cx,int cy,UINT flags)
{
    TRACE("SetWindowPos %p %p %d %d %d %d %u\n", h, h2, x, y, cx, cy, flags);
    return NtUserSetWindowPos(h, h2, x, y, cx, cy, flags);
}

static INT WINAPI my_GetSystemMetrics(INT index)
{
    TRACE("GetSystemMetrics %d\n", index);
    return NtUserGetSystemMetrics( index );
}

static LONG_PTR WINAPI my_SetWindowLongPtrW(HWND hwnd, INT offset, LONG_PTR newval)
{
    TRACE("SetWindowLongPtrW %p %d %ld\n", hwnd, offset, newval);
    /* ignore possibility of DWLP_DLGPROC */
    return NtUserSetWindowLongPtr( hwnd, offset, newval, FALSE );
}

DECLSPEC_EXPORT struct macdrv_functions_t macdrv_functions =
{
    &my_macdrv_init_display_devices,
    &my_get_win_data,
    &my_release_win_data,
    &my_macdrv_get_cocoa_window,
    &my_macdrv_create_metal_device,
    &my_macdrv_release_metal_device,
    &my_macdrv_view_create_metal_view,
    &my_macdrv_view_get_metal_layer,
    &my_macdrv_view_release_metal_view,
    &my_OnMainThread,
    &my_RegQueryValueExA,
    &my_RegSetValueExA,
    &my_RegOpenKeyExA,
    &my_RegCreateKeyExA,
    &my_RegCloseKey,
    &my_EnumDisplayMonitors,
    &my_GetMonitorInfoA,
    &my_AdjustWindowRectEx,
    &my_GetWindowLongPtrW,
    &my_GetWindowRect,
    &my_MoveWindow,
    &my_SetWindowPos,
    &my_GetSystemMetrics,
    &my_SetWindowLongPtrW,
};

void macdrv_client_surface_presented(const macdrv_event *event)
{
    TRACE("client_surface %p\n", event->client_surface_presented.client_surface);

    client_surface_present(event->client_surface_presented.client_surface);
}

static void cf_client_surface_release(CFAllocatorRef allocator, const void *client_surface)
{
    client_surface_release((struct client_surface *)client_surface);
}

#endif
