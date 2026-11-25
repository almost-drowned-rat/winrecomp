/*
 * winrecomp runtime - DINPUT8 (DirectInput 8, ANSI interfaces).
 *
 * COM objects live in guest memory: [0] vtable, [4] refcount, [8] kind.
 * Their vtables hold synthetic addresses of the native methods below, so
 * the guest's `call [eax+0Ch]` lands here like any other import.
 *
 * Keyboard: immediate (GetDeviceState, 256 DIK bytes) and buffered
 * (GetDeviceData, generated from state changes). Game controllers are
 * enumerated through SDL gamepads (M5); until then none are reported.
 */
#include "w32_internal.h"
#include "../platform/wr_platform.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#define DI_OK          0u
#define DI_NOEFFECT    1u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOINTERFACE  0x80004002u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_UNSUPPORTED  0x80004001u
#define DI_BUFFEROVERFLOW  1u

/* ---- COM plumbing ----------------------------------------------------------------- */

static uint32_t make_vtbl(const wr_handler_t *fns, int n, const char *const *names)
{
    uint32_t v = wr_galloc(4u * (uint32_t)n);
    for (int i = 0; i < n; i++)
        wr_wr32(v + 4u * (uint32_t)i, wr_thunk_for(fns[i], names[i]));
    return v;
}

enum { KIND_DI = 1, KIND_KEYBOARD = 2, KIND_MOUSE = 3 };

static uint32_t com_new(uint32_t vtbl, uint32_t kind)
{
    uint32_t o = wr_galloc(16);
    wr_wr32(o + 0, vtbl);
    wr_wr32(o + 4, 1);
    wr_wr32(o + 8, kind);
    return o;
}

WR_HANDLER(com_AddRef)
{
    (void)base;
    uint32_t o = WR_ARG(0);
    uint32_t r = wr_rd32(o + 4) + 1;
    wr_wr32(o + 4, r);
    WR_RET(r, 1);
}

WR_HANDLER(com_Release)
{
    (void)base;
    uint32_t o = WR_ARG(0);
    uint32_t r = wr_rd32(o + 4);
    if (r)
        r--;
    wr_wr32(o + 4, r);
    /* Objects are kept: the guest may still hold its pointer. */
    WR_RET(r, 1);
}

WR_HANDLER(com_QueryInterface)
{
    (void)base;
    uint32_t o = WR_ARG(0), ppv = WR_ARG(2);
    /* The same object answers every DirectInput interface the game asks for. */
    if (ppv)
        wr_wr32(ppv, o);
    wr_wr32(o + 4, wr_rd32(o + 4) + 1);
    WR_RET(0, 3);
}

/* ---- keyboard state ------------------------------------------------------------------ */

static uint8_t g_kb_prev[256];
static uint32_t g_kb_seq;

typedef struct kbev { uint8_t ofs, down; uint32_t t; } kbev;
static kbev g_kb_buf[256];
static int g_kb_n, g_kb_bufsize = 0;

static void kb_poll(void)
{
    if (wr_platform_headless())
        return;
    SDL_PumpEvents();
    uint8_t cur[256];
    wr_keys_dik_state(cur);
    if (g_kb_bufsize) {
        for (int i = 0; i < 256; i++)
            if ((cur[i] ^ g_kb_prev[i]) & 0x80) {
                if (g_kb_n < 256)
                    g_kb_buf[g_kb_n++] = (kbev){ (uint8_t)i, cur[i], wr_time_ms() };
            }
    }
    memcpy(g_kb_prev, cur, 256);
}

/* ---- IDirectInputDevice8A ---------------------------------------------------------- */

WR_HANDLER(dev_GetCapabilities)
{
    (void)base;
    uint32_t o = WR_ARG(0), caps = WR_ARG(1);
    uint32_t size = wr_rd32(caps);
    if (size < 24)
        WR_RET(DIERR_INVALIDPARAM, 2);
    memset((uint8_t *)wr_ptr(caps) + 4, 0, size - 4);
    wr_wr32(caps + 4, 1);   /* DIDC_ATTACHED */
    if (wr_rd32(o + 8) == KIND_KEYBOARD) {
        wr_wr32(caps + 8, 0x13u | (4u << 8));   /* DI8DEVTYPE_KEYBOARD, DI8DEVTYPEKEYBOARD_PCENH */
        wr_wr32(caps + 16, 128);                /* buttons */
    } else {
        wr_wr32(caps + 8, 0x12u);               /* DI8DEVTYPE_MOUSE */
        wr_wr32(caps + 12, 3);
        wr_wr32(caps + 16, 3);
    }
    WR_RET(DI_OK, 2);
}

WR_HANDLER(dev_EnumObjects) { (void)base; WR_RET(DI_OK, 4); }

WR_HANDLER(dev_GetProperty) { (void)base; WR_RET(DIERR_UNSUPPORTED, 3); }

WR_HANDLER(dev_SetProperty)
{
    (void)base;
    uint32_t guid = WR_ARG(1), ph = WR_ARG(2);
    /* DIPROP_BUFFERSIZE is MAKEDIPROP(1): the GUID "pointer" is the value 1. */
    if (guid == 1 && ph) {
        uint32_t v = wr_rd32(ph + 16);
        g_kb_bufsize = v > 256 ? 256 : (int)v;
        wr_log("DirectInput: keyboard buffer size %u", v);
    }
    WR_RET(DI_OK, 3);
}

WR_HANDLER(dev_Acquire) { (void)base; kb_poll(); g_kb_n = 0; WR_RET(DI_OK, 1); }
WR_HANDLER(dev_Unacquire) { (void)base; WR_RET(DI_OK, 1); }

WR_HANDLER(dev_GetDeviceState)
{
    (void)base;
    uint32_t o = WR_ARG(0), n = WR_ARG(1), out = WR_ARG(2);
    if (!wr_mem_valid(out, n))
        WR_RET(DIERR_INVALIDPARAM, 3);
    memset(wr_ptr(out), 0, n);
    if (wr_rd32(o + 8) == KIND_KEYBOARD) {
        kb_poll();
        memcpy(wr_ptr(out), g_kb_prev, n < 256 ? n : 256);
    }
    WR_RET(DI_OK, 3);
}

WR_HANDLER(dev_GetDeviceData)
{
    (void)base;
    uint32_t o = WR_ARG(0), objsize = WR_ARG(1), out = WR_ARG(2), pinout = WR_ARG(3), flags = WR_ARG(4);
    uint32_t want = wr_rd32(pinout);
    if (wr_rd32(o + 8) != KIND_KEYBOARD) {
        wr_wr32(pinout, 0);
        WR_RET(DI_OK, 5);
    }
    kb_poll();
    uint32_t n = 0;
    int peek = (flags & 1u) != 0;   /* DIGDD_PEEK */
    if (!out) {                     /* flush or count */
        n = (uint32_t)g_kb_n < want ? (uint32_t)g_kb_n : want;
        if (!peek) {
            memmove(g_kb_buf, g_kb_buf + n, (size_t)(g_kb_n - (int)n) * sizeof g_kb_buf[0]);
            g_kb_n -= (int)n;
        }
        wr_wr32(pinout, n);
        WR_RET(DI_OK, 5);
    }
    for (; n < want && (int)n < g_kb_n; n++) {
        uint32_t r = out + n * objsize;
        wr_wr32(r + 0, g_kb_buf[n].ofs);
        wr_wr32(r + 4, g_kb_buf[n].down);
        wr_wr32(r + 8, g_kb_buf[n].t);
        wr_wr32(r + 12, ++g_kb_seq);
        if (objsize >= 20)
            wr_wr32(r + 16, 0);
    }
    if (!peek) {
        memmove(g_kb_buf, g_kb_buf + n, (size_t)(g_kb_n - (int)n) * sizeof g_kb_buf[0]);
        g_kb_n -= (int)n;
    }
    wr_wr32(pinout, n);
    WR_RET(DI_OK, 5);
}

WR_HANDLER(dev_SetDataFormat) { (void)base; WR_RET(DI_OK, 2); }
WR_HANDLER(dev_SetEventNotification) { (void)base; WR_RET(DI_OK, 2); }
WR_HANDLER(dev_SetCooperativeLevel) { (void)base; WR_RET(DI_OK, 3); }
WR_HANDLER(dev_GetObjectInfo) { (void)base; WR_RET(DIERR_UNSUPPORTED, 4); }

WR_HANDLER(dev_GetDeviceInfo)
{
    (void)base;
    uint32_t o = WR_ARG(0), p = WR_ARG(1);
    uint32_t size = wr_rd32(p);
    if (size < 44)
        WR_RET(DIERR_INVALIDPARAM, 2);
    memset((uint8_t *)wr_ptr(p) + 4, 0, size - 4);
    int kb = wr_rd32(o + 8) == KIND_KEYBOARD;
    wr_wr32(p + 36, kb ? 0x413u : 0x112u);      /* dwDevType */
    wr_put_str(p + 40, 260, kb ? "Keyboard" : "Mouse");
    if (size >= 300)
        wr_put_str(p + 300, 260, kb ? "Keyboard" : "Mouse");
    WR_RET(DI_OK, 2);
}

WR_HANDLER(dev_RunControlPanel) { (void)base; WR_RET(DI_OK, 3); }
WR_HANDLER(dev_Initialize) { (void)base; WR_RET(DI_OK, 4); }
WR_HANDLER(dev_CreateEffect) { (void)base; if (WR_ARG(3)) wr_wr32(WR_ARG(3), 0); WR_RET(DIERR_UNSUPPORTED, 5); }
WR_HANDLER(dev_EnumEffects) { (void)base; WR_RET(DI_OK, 4); }
WR_HANDLER(dev_GetEffectInfo) { (void)base; WR_RET(DIERR_UNSUPPORTED, 3); }
WR_HANDLER(dev_GetForceFeedbackState) { (void)base; WR_RET(DIERR_UNSUPPORTED, 2); }
WR_HANDLER(dev_SendForceFeedbackCommand) { (void)base; WR_RET(DIERR_UNSUPPORTED, 2); }
WR_HANDLER(dev_EnumCreatedEffectObjects) { (void)base; WR_RET(DI_OK, 4); }
WR_HANDLER(dev_Escape) { (void)base; WR_RET(DIERR_UNSUPPORTED, 2); }
WR_HANDLER(dev_Poll) { (void)base; kb_poll(); WR_RET(DI_NOEFFECT, 1); }
WR_HANDLER(dev_SendDeviceData) { (void)base; WR_RET(DIERR_UNSUPPORTED, 5); }
WR_HANDLER(dev_EnumEffectsInFile) { (void)base; WR_RET(DIERR_UNSUPPORTED, 5); }
WR_HANDLER(dev_WriteEffectToFile) { (void)base; WR_RET(DIERR_UNSUPPORTED, 5); }
WR_HANDLER(dev_BuildActionMap) { (void)base; WR_RET(DIERR_UNSUPPORTED, 4); }
WR_HANDLER(dev_SetActionMap) { (void)base; WR_RET(DIERR_UNSUPPORTED, 4); }
WR_HANDLER(dev_GetImageInfo) { (void)base; WR_RET(DIERR_UNSUPPORTED, 2); }

static const wr_handler_t k_dev_fns[] = {
    com_QueryInterface, com_AddRef, com_Release, dev_GetCapabilities, dev_EnumObjects, dev_GetProperty,
    dev_SetProperty, dev_Acquire, dev_Unacquire, dev_GetDeviceState, dev_GetDeviceData, dev_SetDataFormat,
    dev_SetEventNotification, dev_SetCooperativeLevel, dev_GetObjectInfo, dev_GetDeviceInfo,
    dev_RunControlPanel, dev_Initialize, dev_CreateEffect, dev_EnumEffects, dev_GetEffectInfo,
    dev_GetForceFeedbackState, dev_SendForceFeedbackCommand, dev_EnumCreatedEffectObjects, dev_Escape,
    dev_Poll, dev_SendDeviceData, dev_EnumEffectsInFile, dev_WriteEffectToFile, dev_BuildActionMap,
    dev_SetActionMap, dev_GetImageInfo,
};
static const char *const k_dev_names[] = {
    "IDirectInputDevice8A::QueryInterface", "IDirectInputDevice8A::AddRef", "IDirectInputDevice8A::Release",
    "IDirectInputDevice8A::GetCapabilities", "IDirectInputDevice8A::EnumObjects",
    "IDirectInputDevice8A::GetProperty", "IDirectInputDevice8A::SetProperty", "IDirectInputDevice8A::Acquire",
    "IDirectInputDevice8A::Unacquire", "IDirectInputDevice8A::GetDeviceState",
    "IDirectInputDevice8A::GetDeviceData", "IDirectInputDevice8A::SetDataFormat",
    "IDirectInputDevice8A::SetEventNotification", "IDirectInputDevice8A::SetCooperativeLevel",
    "IDirectInputDevice8A::GetObjectInfo", "IDirectInputDevice8A::GetDeviceInfo",
    "IDirectInputDevice8A::RunControlPanel", "IDirectInputDevice8A::Initialize",
    "IDirectInputDevice8A::CreateEffect", "IDirectInputDevice8A::EnumEffects",
    "IDirectInputDevice8A::GetEffectInfo", "IDirectInputDevice8A::GetForceFeedbackState",
    "IDirectInputDevice8A::SendForceFeedbackCommand", "IDirectInputDevice8A::EnumCreatedEffectObjects",
    "IDirectInputDevice8A::Escape", "IDirectInputDevice8A::Poll", "IDirectInputDevice8A::SendDeviceData",
    "IDirectInputDevice8A::EnumEffectsInFile", "IDirectInputDevice8A::WriteEffectToFile",
    "IDirectInputDevice8A::BuildActionMap", "IDirectInputDevice8A::SetActionMap",
    "IDirectInputDevice8A::GetImageInfo",
};

static uint32_t g_dev_vtbl;

/* ---- IDirectInput8A ------------------------------------------------------------------- */

WR_HANDLER(di_CreateDevice)
{
    (void)base;
    uint32_t guid = WR_ARG(1), out = WR_ARG(2);
    uint32_t d1 = wr_rd32(guid);
    if (!g_dev_vtbl)
        g_dev_vtbl = make_vtbl(k_dev_fns, (int)(sizeof k_dev_fns / sizeof k_dev_fns[0]), k_dev_names);
    if (d1 == 0x6F1D2B61u) {   /* GUID_SysKeyboard */
        wr_wr32(out, com_new(g_dev_vtbl, KIND_KEYBOARD));
        wr_log("DirectInput: keyboard device created");
        WR_RET(DI_OK, 4);
    }
    if (d1 == 0x6F1D2B60u) {   /* GUID_SysMouse */
        wr_wr32(out, com_new(g_dev_vtbl, KIND_MOUSE));
        WR_RET(DI_OK, 4);
    }
    wr_log("DirectInput: CreateDevice for unknown GUID %08X", d1);
    wr_wr32(out, 0);
    WR_RET(DIERR_DEVICENOTREG, 4);
}

WR_HANDLER(di_EnumDevices)
{
    (void)base;
    wr_log("DirectInput: EnumDevices(type %u): no game controllers yet", WR_ARG(1));
    WR_RET(DI_OK, 5);
}

WR_HANDLER(di_GetDeviceStatus)
{
    (void)base;
    uint32_t d1 = wr_rd32(WR_ARG(1));
    WR_RET(d1 == 0x6F1D2B61u || d1 == 0x6F1D2B60u ? DI_OK : DIERR_DEVICENOTREG, 2);
}

WR_HANDLER(di_RunControlPanel) { (void)base; WR_RET(DI_OK, 3); }
WR_HANDLER(di_Initialize) { (void)base; WR_RET(DI_OK, 3); }
WR_HANDLER(di_FindDevice) { (void)base; WR_RET(DIERR_DEVICENOTREG, 4); }
WR_HANDLER(di_EnumDevicesBySemantics) { (void)base; WR_RET(DI_OK, 6); }
WR_HANDLER(di_ConfigureDevices) { (void)base; WR_RET(DIERR_UNSUPPORTED, 5); }

static const wr_handler_t k_di_fns[] = {
    com_QueryInterface, com_AddRef, com_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus,
    di_RunControlPanel, di_Initialize, di_FindDevice, di_EnumDevicesBySemantics, di_ConfigureDevices,
};
static const char *const k_di_names[] = {
    "IDirectInput8A::QueryInterface", "IDirectInput8A::AddRef", "IDirectInput8A::Release",
    "IDirectInput8A::CreateDevice", "IDirectInput8A::EnumDevices", "IDirectInput8A::GetDeviceStatus",
    "IDirectInput8A::RunControlPanel", "IDirectInput8A::Initialize", "IDirectInput8A::FindDevice",
    "IDirectInput8A::EnumDevicesBySemantics", "IDirectInput8A::ConfigureDevices",
};

/* DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter) */
WR_HANDLER(di8_DirectInput8Create)
{
    (void)base;
    static uint32_t vtbl;
    if (!vtbl)
        vtbl = make_vtbl(k_di_fns, (int)(sizeof k_di_fns / sizeof k_di_fns[0]), k_di_names);
    wr_wr32(WR_ARG(3), com_new(vtbl, KIND_DI));
    wr_log("DirectInput8Create(version 0x%X)", WR_ARG(1));
    WR_RET(DI_OK, 5);
}

static const wr_export di8_exports[] = { { "DirectInput8Create", di8_DirectInput8Create }, { NULL, NULL } };
static wr_module di8_mod = { "DINPUT8.dll", di8_exports, 0 };

void w32_register_dinput8(void) { wr_module_register(&di8_mod); }
