// Minimal DirectPlay 8 definitions needed to stand in for IDirectPlay8Peer.
// Written from the documented DirectX 8/9 ABI; only what KnightShift uses.
#pragma once
#include <windows.h>
#include <stdint.h>

typedef DWORD DPNID;
typedef DWORD DPNHANDLE;
typedef HRESULT(WINAPI* PFNDPNMESSAGEHANDLER)(PVOID pvUserContext, DWORD dwMessageType, PVOID pMessage);

#define DPN_HR(code) ((HRESULT)(0x80158000u + (code)))
static const HRESULT DPNSUCCESS_PENDING_ = (HRESULT)0x0015800E;
static const HRESULT DPNERR_ALREADYCONNECTED_ = DPN_HR(0x060);
static const HRESULT DPNERR_ALREADYINITIALIZED_ = DPN_HR(0x080);
static const HRESULT DPNERR_BUFFERTOOSMALL_ = DPN_HR(0x100);
static const HRESULT DPNERR_CONNECTING_ = DPN_HR(0x150);
static const HRESULT DPNERR_CONNECTIONLOST_ = DPN_HR(0x160);
static const HRESULT DPNERR_HOSTREJECTEDCONNECTION_ = DPN_HR(0x260);
static const HRESULT DPNERR_HOSTTERMINATEDSESSION_ = DPN_HR(0x270);
static const HRESULT DPNERR_INVALIDAPPLICATION_ = DPN_HR(0x300);
static const HRESULT DPNERR_INVALIDHANDLE_ = DPN_HR(0x360);
static const HRESULT DPNERR_INVALIDPASSWORD_ = DPN_HR(0x410);
static const HRESULT DPNERR_INVALIDPLAYER_ = DPN_HR(0x420);
static const HRESULT DPNERR_NOCONNECTION_ = DPN_HR(0x480);
static const HRESULT DPNERR_NOTHOST_ = DPN_HR(0x530);
static const HRESULT DPNERR_SESSIONFULL_ = DPN_HR(0x610);
static const HRESULT DPNERR_TIMEDOUT_ = DPN_HR(0x630);
static const HRESULT DPNERR_UNINITIALIZED_ = DPN_HR(0x640);
static const HRESULT DPNERR_USERCANCEL_ = DPN_HR(0x650);

enum : DWORD {
    DPN_MSGID_ADD_PLAYER_TO_GROUP_ = 0xFFFF0001,
    DPN_MSGID_APPLICATION_DESC_ = 0xFFFF0002,
    DPN_MSGID_ASYNC_OP_COMPLETE_ = 0xFFFF0003,
    DPN_MSGID_CONNECT_COMPLETE_ = 0xFFFF0005,
    DPN_MSGID_CREATE_PLAYER_ = 0xFFFF0007,
    DPN_MSGID_DESTROY_PLAYER_ = 0xFFFF0009,
    DPN_MSGID_ENUM_HOSTS_RESPONSE_ = 0xFFFF000B,
    DPN_MSGID_HOST_MIGRATE_ = 0xFFFF000D,
    DPN_MSGID_INDICATE_CONNECT_ = 0xFFFF000E,
    DPN_MSGID_PEER_INFO_ = 0xFFFF0010,
    DPN_MSGID_RECEIVE_ = 0xFFFF0011,
    DPN_MSGID_RETURN_BUFFER_ = 0xFFFF0013,
    DPN_MSGID_SEND_COMPLETE_ = 0xFFFF0014,
    DPN_MSGID_TERMINATE_SESSION_ = 0xFFFF0016,
};

enum : DWORD {
    DPNOP_SYNC_ = 0x80000000,
    DPNSEND_NOCOMPLETE_ = 0x0002,
    DPNSEND_GUARANTEED_ = 0x0008,
    DPNSEND_NOLOOPBACK_ = 0x0020,
    DPNCANCEL_CONNECT_ = 0x0001,
    DPNCANCEL_ENUM_ = 0x0002,
    DPNCANCEL_SEND_ = 0x0004,
    DPNCANCEL_ALL_OPERATIONS_ = 0x8000,
    DPNINFO_NAME_ = 0x0001,
    DPNINFO_DATA_ = 0x0002,
    DPNPLAYER_LOCAL_ = 0x0002,
    DPNPLAYER_HOST_ = 0x0004,
    DPNSESSION_REQUIREPASSWORD_ = 0x0080,
    DPNDESTROYPLAYERREASON_NORMAL_ = 1,
    DPNDESTROYPLAYERREASON_CONNECTIONLOST_ = 2,
    DPNDESTROYPLAYERREASON_SESSIONTERMINATED_ = 3,
    DPNDESTROYPLAYERREASON_HOSTDESTROYEDPLAYER_ = 4,
    DPNA_DATATYPE_STRING_ = 1,
    DPNA_DATATYPE_DWORD_ = 2,
};

#pragma pack(push, 4)
struct DPN_APPLICATION_DESC_ {
    DWORD dwSize;
    DWORD dwFlags;
    GUID guidInstance;
    GUID guidApplication;
    DWORD dwMaxPlayers;
    DWORD dwCurrentPlayers;
    WCHAR* pwszSessionName;
    WCHAR* pwszPassword;
    PVOID pvReservedData;
    DWORD dwReservedDataSize;
    PVOID pvApplicationReservedData;
    DWORD dwApplicationReservedDataSize;
};
static_assert(sizeof(DPN_APPLICATION_DESC_) == 0x48, "app desc");

struct DPN_PLAYER_INFO_ {
    DWORD dwSize;
    DWORD dwInfoFlags;
    PWSTR pwszName;
    PVOID pvData;
    DWORD dwDataSize;
    DWORD dwPlayerFlags;
};
static_assert(sizeof(DPN_PLAYER_INFO_) == 0x18, "player info");

struct DPN_BUFFER_DESC_ {
    DWORD dwBufferSize;
    BYTE* pBufferData;
};

struct DPN_SERVICE_PROVIDER_INFO_ {
    DWORD dwFlags;
    GUID guid;
    WCHAR* pwszName;
    PVOID pvReserved;
    DWORD dwReserved;
};
static_assert(sizeof(DPN_SERVICE_PROVIDER_INFO_) == 32, "sp info");

struct DPN_SP_CAPS_ {
    DWORD dwSize, dwFlags, dwNumThreads, dwDefaultEnumCount, dwDefaultEnumRetryInterval, dwDefaultEnumTimeout,
        dwMaxEnumPayloadSize, dwBuffersPerThread, dwSystemBufferSize;
};

struct DPN_CONNECTION_INFO_ {
    DWORD dwSize, dwRoundTripLatencyMS, dwThroughputBPS, dwPeakThroughputBPS;
    DWORD dwBytesSentGuaranteed, dwPacketsSentGuaranteed, dwBytesSentNonGuaranteed, dwPacketsSentNonGuaranteed;
    DWORD dwBytesRetried, dwPacketsRetried, dwBytesDropped, dwPacketsDropped;
    DWORD dwMessagesTransmittedHighPriority, dwMessagesTimedOutHighPriority;
    DWORD dwMessagesTransmittedNormalPriority, dwMessagesTimedOutNormalPriority;
    DWORD dwMessagesTransmittedLowPriority, dwMessagesTimedOutLowPriority;
    DWORD dwBytesReceivedGuaranteed, dwPacketsReceivedGuaranteed, dwBytesReceivedNonGuaranteed;
    DWORD dwPacketsReceivedNonGuaranteed, dwMessagesReceived;
};

struct IDirectPlay8Address_;

struct DPNMSG_CREATE_PLAYER_ { DWORD dwSize; DPNID dpnidPlayer; PVOID pvPlayerContext; };
struct DPNMSG_DESTROY_PLAYER_ { DWORD dwSize; DPNID dpnidPlayer; PVOID pvPlayerContext; DWORD dwReason; };
struct DPNMSG_PEER_INFO_ { DWORD dwSize; DPNID dpnidPeer; PVOID pvPlayerContext; };
struct DPNMSG_RECEIVE_ {
    DWORD dwSize; DPNID dpnidSender; PVOID pvPlayerContext; PBYTE pReceiveData; DWORD dwReceiveDataSize;
    DPNHANDLE hBufferHandle;
};
struct DPNMSG_SEND_COMPLETE_ {
    DWORD dwSize; DPNHANDLE hAsyncOp; PVOID pvUserContext; HRESULT hResultCode; DWORD dwSendTime;
    DWORD dwFirstFrameRTT; DWORD dwFirstFrameRetryCount;
};
struct DPNMSG_ASYNC_OP_COMPLETE_ { DWORD dwSize; DPNHANDLE hAsyncOp; PVOID pvUserContext; HRESULT hResultCode; };
struct DPNMSG_CONNECT_COMPLETE_ {
    DWORD dwSize; DPNHANDLE hAsyncOp; PVOID pvUserContext; HRESULT hResultCode; PVOID pvApplicationReplyData;
    DWORD dwApplicationReplyDataSize; DPNID dpnidLocal;
};
struct DPNMSG_ENUM_HOSTS_RESPONSE_ {
    DWORD dwSize; IDirectPlay8Address_* pAddressSender; IDirectPlay8Address_* pAddressDevice;
    const DPN_APPLICATION_DESC_* pApplicationDescription; PVOID pvResponseData; DWORD dwResponseDataSize;
    PVOID pvUserContext; DWORD dwRoundTripLatencyMS;
};
struct DPNMSG_INDICATE_CONNECT_ {
    DWORD dwSize; PVOID pvUserConnectData; DWORD dwUserConnectDataSize; PVOID pvReplyData; DWORD dwReplyDataSize;
    PVOID pvReplyContext; PVOID pvPlayerContext; IDirectPlay8Address_* pAddressPlayer;
    IDirectPlay8Address_* pAddressDevice;
};
struct DPNMSG_TERMINATE_SESSION_ { DWORD dwSize; HRESULT hResultCode; PVOID pvTerminateData; DWORD dwTerminateDataSize; };
struct DPNMSG_RETURN_BUFFER_ { DWORD dwSize; HRESULT hResultCode; PVOID pvBuffer; PVOID pvUserContext; };
#pragma pack(pop)

// {5102DACF-241B-11D3-AEA7-006097B01411}
static const GUID IID_IDirectPlay8Peer_ = {0x5102dacf, 0x241b, 0x11d3, {0xae, 0xa7, 0x00, 0x60, 0x97, 0xb0, 0x14, 0x11}};
// {286F484D-375E-4458-A272-B138E2F80A6A}
static const GUID CLSID_DirectPlay8Peer_ = {0x286f484d, 0x375e, 0x4458, {0xa2, 0x72, 0xb1, 0x38, 0xe2, 0xf8, 0x0a, 0x6a}};
// {934A9523-A3CA-4BC5-ADA0-D6D95D979421}
static const GUID CLSID_DirectPlay8Address_ = {0x934a9523, 0xa3ca, 0x4bc5, {0xad, 0xa0, 0xd6, 0xd9, 0x5d, 0x97, 0x94, 0x21}};
// {83783300-4063-4C8A-9DB3-82830A7FEB31}
static const GUID IID_IDirectPlay8Address_ = {0x83783300, 0x4063, 0x4c8a, {0x9d, 0xb3, 0x82, 0x83, 0x0a, 0x7f, 0xeb, 0x31}};
// {EBFE7BA0-628D-11D2-AE0F-006097B01411}
static const GUID CLSID_DP8SP_TCPIP_ = {0xebfe7ba0, 0x628d, 0x11d2, {0xae, 0x0f, 0x00, 0x60, 0x97, 0xb0, 0x14, 0x11}};
// Device "adapter" reported for the Steam provider.
static const GUID GUID_KSSteamDevice = {0x4b53534e, 0x4554, 0x4b53, {0x53, 0x54, 0x45, 0x41, 0x4d, 0x44, 0x45, 0x56}};

// IDirectPlay8Address (dpnaddr.dll) - we use the real object, only need the vtable shape.
struct IDirectPlay8Address_ {
    virtual HRESULT __stdcall QueryInterface(REFIID, void**) = 0;
    virtual ULONG __stdcall AddRef() = 0;
    virtual ULONG __stdcall Release() = 0;
    virtual HRESULT __stdcall BuildFromURLW(WCHAR*) = 0;
    virtual HRESULT __stdcall BuildFromURLA(CHAR*) = 0;
    virtual HRESULT __stdcall Duplicate(IDirectPlay8Address_**) = 0;
    virtual HRESULT __stdcall SetEqual(IDirectPlay8Address_*) = 0;
    virtual HRESULT __stdcall IsEqual(IDirectPlay8Address_*) = 0;
    virtual HRESULT __stdcall Clear() = 0;
    virtual HRESULT __stdcall GetURLW(WCHAR*, PDWORD) = 0;
    virtual HRESULT __stdcall GetURLA(CHAR*, PDWORD) = 0;
    virtual HRESULT __stdcall GetSP(GUID*) = 0;
    virtual HRESULT __stdcall GetUserData(LPVOID, PDWORD) = 0;
    virtual HRESULT __stdcall SetSP(const GUID*) = 0;
    virtual HRESULT __stdcall SetUserData(const void*, DWORD) = 0;
    virtual HRESULT __stdcall GetNumComponents(PDWORD) = 0;
    virtual HRESULT __stdcall GetComponentByName(const WCHAR*, LPVOID, PDWORD, PDWORD) = 0;
    virtual HRESULT __stdcall GetComponentByIndex(DWORD, WCHAR*, PDWORD, void*, PDWORD, PDWORD) = 0;
    virtual HRESULT __stdcall AddComponent(const WCHAR*, const void*, DWORD, DWORD) = 0;
    virtual HRESULT __stdcall GetDevice(GUID*) = 0;
    virtual HRESULT __stdcall SetDevice(const GUID*) = 0;
    virtual HRESULT __stdcall BuildFromDPADDRESS(LPVOID, DWORD) = 0;
};

// IDirectPlay8Peer vtable, in declaration order.
struct IDirectPlay8Peer_ {
    virtual HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) = 0;
    virtual ULONG __stdcall AddRef() = 0;
    virtual ULONG __stdcall Release() = 0;
    virtual HRESULT __stdcall Initialize(PVOID ctx, PFNDPNMESSAGEHANDLER pfn, DWORD flags) = 0;
    virtual HRESULT __stdcall EnumServiceProviders(const GUID* sp, const GUID* app, DPN_SERVICE_PROVIDER_INFO_* buf,
                                                   PDWORD cb, PDWORD count, DWORD flags) = 0;
    virtual HRESULT __stdcall CancelAsyncOperation(DPNHANDLE h, DWORD flags) = 0;
    virtual HRESULT __stdcall Connect(const DPN_APPLICATION_DESC_* app, IDirectPlay8Address_* host,
                                      IDirectPlay8Address_* dev, const void* sec, const void* cred, const void* data,
                                      DWORD dataSize, void* playerCtx, void* asyncCtx, DPNHANDLE* h, DWORD flags) = 0;
    virtual HRESULT __stdcall SendTo(DPNID to, const DPN_BUFFER_DESC_* bufs, DWORD n, DWORD timeout, void* ctx,
                                     DPNHANDLE* h, DWORD flags) = 0;
    virtual HRESULT __stdcall GetSendQueueInfo(DPNID id, DWORD* msgs, DWORD* bytes, DWORD flags) = 0;
    virtual HRESULT __stdcall Host(const DPN_APPLICATION_DESC_* app, IDirectPlay8Address_** dev, DWORD nDev,
                                   const void* sec, const void* cred, void* playerCtx, DWORD flags) = 0;
    virtual HRESULT __stdcall GetApplicationDesc(DPN_APPLICATION_DESC_* buf, DWORD* size, DWORD flags) = 0;
    virtual HRESULT __stdcall SetApplicationDesc(const DPN_APPLICATION_DESC_* app, DWORD flags) = 0;
    virtual HRESULT __stdcall CreateGroup(const void*, void*, void*, DPNHANDLE*, DWORD) = 0;
    virtual HRESULT __stdcall DestroyGroup(DPNID, void*, DPNHANDLE*, DWORD) = 0;
    virtual HRESULT __stdcall AddPlayerToGroup(DPNID, DPNID, void*, DPNHANDLE*, DWORD) = 0;
    virtual HRESULT __stdcall RemovePlayerFromGroup(DPNID, DPNID, void*, DPNHANDLE*, DWORD) = 0;
    virtual HRESULT __stdcall SetGroupInfo(DPNID, void*, void*, DPNHANDLE*, DWORD) = 0;
    virtual HRESULT __stdcall GetGroupInfo(DPNID, void*, DWORD*, DWORD) = 0;
    virtual HRESULT __stdcall EnumPlayersAndGroups(DPNID* ids, DWORD* count, DWORD flags) = 0;
    virtual HRESULT __stdcall EnumGroupMembers(DPNID, DPNID*, DWORD*, DWORD) = 0;
    virtual HRESULT __stdcall SetPeerInfo(const DPN_PLAYER_INFO_* info, void* ctx, DPNHANDLE* h, DWORD flags) = 0;
    virtual HRESULT __stdcall GetPeerInfo(DPNID id, DPN_PLAYER_INFO_* buf, DWORD* size, DWORD flags) = 0;
    virtual HRESULT __stdcall GetPeerAddress(DPNID id, IDirectPlay8Address_** addr, DWORD flags) = 0;
    virtual HRESULT __stdcall GetLocalHostAddresses(IDirectPlay8Address_** addrs, DWORD* count, DWORD flags) = 0;
    virtual HRESULT __stdcall Close(DWORD flags) = 0;
    virtual HRESULT __stdcall EnumHosts(DPN_APPLICATION_DESC_* app, IDirectPlay8Address_* host,
                                        IDirectPlay8Address_* dev, void* data, DWORD dataSize, DWORD count,
                                        DWORD retry, DWORD timeout, void* ctx, DPNHANDLE* h, DWORD flags) = 0;
    virtual HRESULT __stdcall DestroyPeer(DPNID id, const void* data, DWORD size, DWORD flags) = 0;
    virtual HRESULT __stdcall ReturnBuffer(DPNHANDLE h, DWORD flags) = 0;
    virtual HRESULT __stdcall GetPlayerContext(DPNID id, void** ctx, DWORD flags) = 0;
    virtual HRESULT __stdcall GetGroupContext(DPNID, void**, DWORD) = 0;
    virtual HRESULT __stdcall GetCaps(void* caps, DWORD flags) = 0;
    virtual HRESULT __stdcall SetCaps(const void* caps, DWORD flags) = 0;
    virtual HRESULT __stdcall SetSPCaps(const GUID* sp, const DPN_SP_CAPS_* caps, DWORD flags) = 0;
    virtual HRESULT __stdcall GetSPCaps(const GUID* sp, DPN_SP_CAPS_* caps, DWORD flags) = 0;
    virtual HRESULT __stdcall GetConnectionInfo(DPNID id, DPN_CONNECTION_INFO_* info, DWORD flags) = 0;
    virtual HRESULT __stdcall RegisterLobby(DPNHANDLE, void*, DWORD) = 0;
    virtual HRESULT __stdcall TerminateSession(void* data, DWORD size, DWORD flags) = 0;
};
