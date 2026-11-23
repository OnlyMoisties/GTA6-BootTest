/* One source for the five small audio/texture/storage stub DLLs; pick with /DWHICH_x */
#if defined(WHICH_chad)
#define STUBDLL "chad"
#include "stub.h"
STUB(XDspActivate, E_FAIL_) STUB(XDspConnectWithMaximumStreamLimit, E_FAIL_) STUB(XDspDeactivate, E_FAIL_) STUB(XDspDisconnect, E_FAIL_) STUB(XDspSubmitCommand, E_FAIL_)
#elif defined(WHICH_xg_xs)
#define STUBDLL "xg_xs"
#include "stub.h"
STUB(XGCreateTextureComputer, E_FAIL_) STUB(XGSuggestSwizzleMode, 0)
#elif defined(WHICH_XCRDAPI)
#define STUBDLL "XCRDAPI"
#include "stub.h"
EXP uint64_t DStorageGetFactory(void* riid, void** ppv) { if (ppv) *ppv = NULL; SLOG("DStorageGetFactory", riid, ppv, 0, 0, 0, 0x80004002ull); return 0x80004002ull; }
#elif defined(WHICH_xapu)
#define STUBDLL "xapu"
#include "stub.h"
STUB(XApuConnect, E_FAIL_) STUB(XApuDequeueResult, E_FAIL_) STUB(XApuDisconnect, E_FAIL_) STUB(XApuEnqueueCommand, E_FAIL_) STUB(XApuGetResultCount, 0)
#elif defined(WHICH_AcpHal)
#define STUBDLL "AcpHal"
#include "stub.h"
STUB(AcpHalAllocateShapeContexts, E_FAIL_) STUB(AcpHalCreate, E_FAIL_) STUB(AcpHalReleaseShapeContexts, E_FAIL_) STUB(ApuAlloc, E_FAIL_) STUB(ApuCreateHeap, E_FAIL_) STUB(ApuFree, E_FAIL_) STUB(ApuIsVirtualAddressValid, 0) STUB(ApuMapApuAddress, 0) STUB(ApuMapVirtualAddress, 0)
#endif
