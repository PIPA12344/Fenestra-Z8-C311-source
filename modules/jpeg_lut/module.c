#include "fenestra_sdk.h"
#include "fenestra_memory.h"

enum { PAGE, OPEN, QUERY, DRAW, POLL, ARM, CANCEL, DRAW_XM6, ARM_XM6, DRAW_FILE, ARM_FILE };
enum { IDLE, ARMED, RUNNING, APPLIED, SKIPPED, RETAINED, LOADING, PREPARING, CLEANUP, PREVIEWS };
enum { CARD = 1, FORMAT, MEMORY, LAYOUT, ENCODE, CLOSE, TIMING, PAIRING, DSP_ERROR, CUBE_ERROR };
enum { LUT_STATUS = 1, LUT_ARM, LUT_CANCEL, LUT_ARM_XM6, LUT_ARM_FILE };

static const char *status(uint32_t phase, uint32_t reason, uint32_t xm6) {
    switch (phase) {
        case IDLE: return "Place LOOK.CUBE in FENESTRA";
        case ARMED:
            if (xm6 == 2) return "LOOK.CUBE ready (DSP)";
            if (xm6 == 3) return "LOOK.CUBE ready (3D, CPU)";
            if (xm6) return reason == LAYOUT ? "Use FX L or DX S, SDR 3:2" : "XM6 ready for one SDR JPEG";
            return reason == LAYOUT ? "Waiting for DX 3:2 SDR JPEG S" : "Ready for one DX SDR JPEG S";
        case RUNNING: return "Processing";
        case APPLIED: return "LUT applied to JPEG and previews";
        case RETAINED: return "Restart camera";
        case LOADING: return "Reading LOOK.CUBE";
        case PREPARING: return xm6 == 1 ? "Preparing XM6 buffers" : "Preparing color table";
        case CLEANUP: case PREVIEWS: return "Finishing JPEG and previews";
        default: break;
    }
    switch (reason) {
        case CARD: return "Cannot read LOOK.CUBE";
        case FORMAT: return "Use a Standard 3D CUBE, 2-33 grid";
        case MEMORY: return "Not enough camera memory";
        case LAYOUT: return "Use DX 3:2 SDR JPEG S";
        case ENCODE: return "JPEG encoding failed";
        case CLOSE: return "Restart camera";
        case TIMING: return "Time limit reached; check capture";
        case PAIRING: return "JPEG previews not synchronized";
        case DSP_ERROR: return "XM6 task did not complete";
        case CUBE_ERROR: return "3D LUT frame did not complete";
        default: return "LUT unavailable";
    }
}

static void copy(char *out, const char *text, unsigned size) {
    unsigned i = 0;
    while (i + 1u < size && text[i]) { out[i] = text[i]; ++i; }
    out[i] = 0;
}

int fenestra_module_entry(const FxModuleServices *base) {
    static FxSdkClient client;
    static uint64_t binding;
    static uint32_t stage, phase, reason, xm6, revision, xm6_revision, file_revision;
    static uint32_t last_phase = UINT32_MAX, last_reason = UINT32_MAX;
    static int unavailable;
    if (!base || base->bytes != sizeof(FxServiceModule)) return 1;
    const FxServiceModule *api = (const FxServiceModule *)base;
    if (api->service_abi != FX_SERVICE_ABI) return 2;
    const unsigned required = FX_SDK_MENU | FX_SDK_BINDING;
    if ((api->capabilities & required) != required) return 0;

    if (client.pending) {
        FxServiceReply reply;
        if (fx_sdk_take(&client, api, &reply) != FX_OK) return 0;
        switch (stage) {
            case PAGE:
                if (reply.result == FX_OK) stage = OPEN;
                else if (reply.result != FX_BUSY) unavailable = 1;
                break;
            case OPEN:
                if (reply.result == FX_OK && reply.value[0]) {
                    binding = reply.value[0];
                    stage = QUERY;
                } else if (reply.result != FX_BUSY) {
                    unavailable = 1;
                    stage = DRAW;
                }
                break;
            case ARM: case ARM_XM6: case ARM_FILE:
            case CANCEL:
                stage = QUERY;
                break;
            case QUERY:
                if (reply.result == FX_OK) {
                    phase = (uint32_t)reply.value[0];
                    reason = (uint32_t)reply.value[1];
                    xm6 = (uint32_t)reply.value[2];
                    stage = phase == last_phase && reason == last_reason ? POLL : DRAW;
                } else if (reply.result != FX_BUSY) {
                    unavailable = 1;
                    stage = DRAW;
                }
                break;
            case DRAW:
                if (reply.result == FX_OK) {
                    revision = (uint32_t)reply.value[0];
                    stage = DRAW_XM6;
                } else if (reply.result != FX_BUSY) {
                    stage = POLL;
                }
                break;
            case DRAW_XM6:
                if (reply.result == FX_OK) {
                    xm6_revision = (uint32_t)reply.value[0];
                    stage = DRAW_FILE;
                } else if (reply.result != FX_BUSY) {
                    stage = DRAW_FILE;
                }
                break;
            case DRAW_FILE:
                if (reply.result == FX_OK) {
                    file_revision = (uint32_t)reply.value[0];
                    last_phase = phase;
                    last_reason = reason;
                    stage = POLL;
                } else if (reply.result != FX_BUSY) {
                    stage = POLL;
                }
                break;
            case POLL:
                stage = unavailable ? POLL : QUERY;
                if (!unavailable && reply.result == FX_OK && reply.bytes == sizeof(FxUiEvent)) {
                    FxUiEvent event;
                    memcpy(&event, reply.data, sizeof(event));
                    if (event.control == 2 && event.reserved == revision && event.event == FX_INPUT_ACTIVATE)
                        stage = phase == ARMED || phase == LOADING || phase == PREPARING ? CANCEL : ARM;
                    if (event.control == 3 && event.reserved == xm6_revision && event.event == FX_INPUT_ACTIVATE)
                        stage = ARM_XM6;
                    if (event.control == 4 && event.reserved == file_revision && event.event == FX_INPUT_ACTIVATE)
                        stage = ARM_FILE;
                }
                break;
            default: return 0;
        }
    }
    if (stage == PAGE && unavailable) return 0;
    if (stage == OPEN) (void)fx_sdk_open(&client, api, "camera.jpeg.lut", 1);
    else if (stage == QUERY || stage == ARM || stage == ARM_XM6 || stage == ARM_FILE || stage == CANCEL) {
        uint32_t op = stage == QUERY ? LUT_STATUS : stage == ARM ? LUT_ARM :
                      stage == ARM_FILE ? LUT_ARM_FILE : stage == ARM_XM6 ? LUT_ARM_XM6 : LUT_CANCEL;
        (void)fx_sdk_platform(&client, api, FX_BIND_CALL, binding, op, 0, NULL, 0);
    } else if (stage == POLL) (void)fx_sdk_events(&client, api);
    else {
        FxUiControl control = {.abi = 1, .bytes = sizeof(control), .flags = FX_UI_VISIBLE};
        if (stage == PAGE) {
            control.id = 1;
            control.kind = FX_UI_PAGE;
            control.flags |= FX_UI_ENABLED;
            copy(control.label, "JPEG LUT", sizeof(control.label));
        } else if (stage == DRAW_XM6 || stage == DRAW_FILE) {
            control.id = stage == DRAW_FILE ? 4u : 3u;
            control.parent = 1;
            control.kind = FX_UI_ACTION;
            if (!unavailable && (phase == IDLE || phase == APPLIED || phase == SKIPPED)) control.flags |= FX_UI_ENABLED;
            copy(control.label, stage == DRAW_FILE ? "Load LOOK.CUBE for FX/DX JPEG" : "XM6 test curve (one JPEG)",
                 sizeof(control.label));
            copy(control.text, stage == DRAW_FILE && xm6 >= 2 ? status(phase, reason, xm6) : "FX L or DX S, SDR 3:2",
                 sizeof(control.text));
        } else {
            control.id = 2;
            control.parent = 1;
            control.kind = FX_UI_ACTION;
            if (!unavailable && phase != RUNNING && phase != CLEANUP && phase != PREVIEWS && phase != RETAINED)
                control.flags |= FX_UI_ENABLED;
            copy(control.label, phase == ARMED || phase == LOADING || phase == PREPARING ? "Cancel LUT" : "Load LUT for DX S (legacy)",
                 sizeof(control.label));
            copy(control.text, unavailable ? "LUT component unavailable" : status(phase, reason, xm6), sizeof(control.text));
        }
        (void)fx_sdk_ui(&client, api, &control);
    }
    return 0;
}
