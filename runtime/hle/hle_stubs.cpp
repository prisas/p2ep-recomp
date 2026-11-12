// Placeholders for system calls whose real implementation has not been written yet. Each logs
// once when first called. This file shrinks as the libraries are implemented and is meant to end
// up empty.

#include "runtime/hle/hle.h"
#include "runtime/hle/hle_display.h"

using namespace p2;
using namespace p2::kernel;

// --- sceCtrl ---
HLE_STUB(sceCtrlSetSamplingCycle, 0)
HLE_STUB(sceCtrlSetSamplingMode, 0)
HLE_STUB(sceCtrlSetIdleCancelThreshold, 0)
HLE(sceCtrlReadBufferPositive) {
    u32 out = ARG(0);
    hle::wait_for_vblank();
    if (mem::valid(out, 16)) {
        mem::w32(out + 0, u32(now_us()));
        mem::w32(out + 4, 0);
        mem::w8(out + 8, 128);
        mem::w8(out + 9, 128);
    }
    RET(1);
}

// --- sceGe_user ---
HLE_STUB(sceGeEdramGetAddr, 0x04000000)
HLE_STUB(sceGeSetCallback, 1)
HLE_STUB(sceGeUnsetCallback, 0)
HLE_STUB(sceGeListEnQueue, 1)
HLE_STUB(sceGeListEnQueueHead, 1)
HLE_STUB(sceGeListUpdateStallAddr, 0)
HLE_STUB(sceGeListSync, 0)
HLE_STUB(sceGeDrawSync, 0)
HLE_STUB(sceGeBreak, 0)
HLE_STUB(sceGeContinue, 0)

// --- sceAudio ---
HLE_STUB(sceAudioChReserve, 0)
HLE_STUB(sceAudioChRelease, 0)
HLE_STUB(sceAudioChangeChannelConfig, 0)
HLE_STUB(sceAudioChangeChannelVolume, 0)
HLE_STUB(sceAudioSetChannelDataLen, 0)
HLE_STUB(sceAudioGetChannelRestLength, 0)
HLE_STUB(sceAudioOutputPanned, 0)
HLE(sceAudioOutputBlocking) {
    wait_current_for(WaitType::Delay, 0, 23220, false); // one 1024-sample buffer at 44.1 kHz
    RET(0);
}
HLE(sceAudioOutputPannedBlocking) {
    wait_current_for(WaitType::Delay, 0, 23220, false);
    RET(0);
}

// --- sceSasCore ---
HLE_STUB(__sceSasInit, 0)
HLE_STUB(__sceSasCore, 0)
HLE_STUB(__sceSasCoreWithMix, 0)
HLE_STUB(__sceSasSetVoice, 0)
HLE_STUB(__sceSasSetVoicePCM, 0)
HLE_STUB(__sceSasSetPitch, 0)
HLE_STUB(__sceSasSetVolume, 0)
HLE_STUB(__sceSasSetADSR, 0)
HLE_STUB(__sceSasSetADSRmode, 0)
HLE_STUB(__sceSasSetSL, 0)
HLE_STUB(__sceSasSetSimpleADSR, 0)
HLE_STUB(__sceSasSetKeyOn, 0)
HLE_STUB(__sceSasSetKeyOff, 0)
HLE_STUB(__sceSasSetPause, 0)
HLE_STUB(__sceSasGetPauseFlag, 0)
HLE_STUB(__sceSasGetEndFlag, 0xFFFFFFFF)
HLE_STUB(__sceSasGetEnvelopeHeight, 0)
HLE_STUB(__sceSasGetAllEnvelopeHeights, 0)
HLE_STUB(__sceSasSetNoise, 0)
HLE_STUB(__sceSasRevType, 0)
HLE_STUB(__sceSasRevParam, 0)
HLE_STUB(__sceSasRevEVOL, 0)
HLE_STUB(__sceSasRevVON, 0)
HLE_STUB(__sceSasSetOutputmode, 0)
HLE_STUB(__sceSasGetOutputmode, 0)
HLE_STUB(__sceSasSetGrain, 0)
HLE_STUB(__sceSasGetGrain, 256)

// --- sceAtrac3plus ---
HLE_STUB(sceAtracSetDataAndGetID, 0)
HLE_STUB(sceAtracReleaseAtracID, 0)
HLE_STUB(sceAtracDecodeData, 0x80630024)
HLE_STUB(sceAtracGetRemainFrame, 0)
HLE_STUB(sceAtracGetStreamDataInfo, 0)
HLE_STUB(sceAtracAddStreamData, 0)
HLE_STUB(sceAtracGetSecondBufferInfo, 0x80630022)
HLE_STUB(sceAtracSetSecondBuffer, 0)
HLE_STUB(sceAtracGetNextDecodePosition, 0)
HLE_STUB(sceAtracGetSoundSample, 0)
HLE_STUB(sceAtracSetLoopNum, 0)
HLE_STUB(sceAtracGetBufferInfoForResetting, 0)
HLE_STUB(sceAtracResetPlayPosition, 0)
HLE_STUB(sceAtracGetInternalErrorInfo, 0)

// --- sceMpeg ---
HLE_STUB(sceMpegInit, 0)
HLE_STUB(sceMpegFinish, 0)
HLE_STUB(sceMpegQueryMemSize, 0x10000)
HLE_STUB(sceMpegCreate, 0)
HLE_STUB(sceMpegDelete, 0)
HLE_STUB(sceMpegRegistStream, 0)
HLE_STUB(sceMpegUnRegistStream, 0)
HLE_STUB(sceMpegMallocAvcEsBuf, 1)
HLE_STUB(sceMpegFreeAvcEsBuf, 0)
HLE_STUB(sceMpegQueryAtracEsSize, 0)
HLE_STUB(sceMpegQueryStreamOffset, 0)
HLE_STUB(sceMpegQueryStreamSize, 0)
HLE_STUB(sceMpegInitAu, 0)
HLE_STUB(sceMpegGetAvcAu, 0x80618001)
HLE_STUB(sceMpegGetAtracAu, 0x80618001)
HLE_STUB(sceMpegFlushAllStream, 0)
HLE_STUB(sceMpegAvcDecodeFlush, 0)
HLE_STUB(sceMpegAvcQueryYCbCrSize, 0)
HLE_STUB(sceMpegAvcInitYCbCr, 0)
HLE_STUB(sceMpegAvcDecodeYCbCr, 0)
HLE_STUB(sceMpegAvcDecodeStopYCbCr, 0)
HLE_STUB(sceMpegAvcCsc, 0)
HLE_STUB(sceMpegAtracDecode, 0)
HLE_STUB(sceMpegRingbufferQueryMemSize, 0x10000)
HLE_STUB(sceMpegRingbufferConstruct, 0)
HLE_STUB(sceMpegRingbufferDestruct, 0)
HLE_STUB(sceMpegRingbufferPut, 0)
HLE_STUB(sceMpegRingbufferAvailableSize, 0)

// --- sceUtility ---
HLE_STUB(sceUtilitySavedataInitStart, 0)
HLE_STUB(sceUtilitySavedataShutdownStart, 0)
HLE_STUB(sceUtilitySavedataUpdate, 0)
HLE_STUB(sceUtilitySavedataGetStatus, 0)
HLE_STUB(sceUtilityMsgDialogInitStart, 0)
HLE_STUB(sceUtilityMsgDialogShutdownStart, 0)
HLE_STUB(sceUtilityMsgDialogUpdate, 0)
HLE_STUB(sceUtilityMsgDialogGetStatus, 0)
HLE_STUB(sceUtilityGamedataInstallInitStart, 0)
HLE_STUB(sceUtilityGamedataInstallShutdownStart, 0)
HLE_STUB(sceUtilityGamedataInstallUpdate, 0)
HLE_STUB(sceUtilityGamedataInstallGetStatus, 0)
HLE_STUB(sceUtilityGamedataInstallAbort, 0)
