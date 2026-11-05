#include "Hooks_Misc.h"
#include "HookMacros.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/Config/LuaConfig.h"
#include "dllmain.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <optional>

namespace {
    // ── Resolve-only functions ─────────────────────────────────────
    RESOLVE_FUNC(CUtlBufferEnsureCapacity, void*, CUtlBuffer* pCUtlBuffer, uint32 newCapacity);

    // ── VEH-captured functions (one-shot int3) ───────────────────────────────
    // On int3 hit, ctx->Rcx is stored to the named output variable.
    CAPTURE_THIS_FUNC(GetAppIDForCurrentPipe, AppId_t,      g_steamEngine,    void*);
    CAPTURE_THIS_FUNC(GetAppDataFromAppInfo,  int64,        g_pCAppInfoCache, void*, AppId_t, const char*, uint8*, int32);

    // Assumes one game at a time.  Set by SpawnProcess VEH when -onlinefix
    // is detected; cleared when a non-onlinefix game launches.
    AppId_t   g_OnlineFixRealAppId;
    // 游戏开始走 SteamNetworkingSockets 之后置位，见 GetAppID 处理器。
    bool      g_NetworkingSocketsActive;
    // 本次启动是否用 -realappid 关掉 appid 翻转，见 ShouldReportOnlineFixAppId。
    bool      g_SuppressAppIdFlip;
    // AppId the -onlinefix game is disguised as for this launch. 480 unless the
    // launch options picked another with -onlinefix=<appid>. Read from Steam's
    // IPC and network threads, so atomic.
    std::atomic<AppId_t> g_OnlineFixFakeAppId{kOnlineFixAppId};

    // Parse the AppId from "-onlinefix=<appid>", "-onlinefix:<appid>" or
    // "-onlinefix <appid>". nullopt when the flag carries no number, in which
    // case the default 480 applies. AppIds live in the low 24 bits of a
    // CGameID, so anything wider is rejected.
    static std::optional<AppId_t> ParseOnlineFixAppId(const char* flag) {
        const char* p = flag + std::strlen("-onlinefix");
        if (*p == '=' || *p == ':') {
            ++p;
        } else if (*p == ' ' || *p == '\t') {
            while (*p == ' ' || *p == '\t') ++p;
        } else {
            return std::nullopt;
        }
        if (!std::isdigit(static_cast<unsigned char>(*p))) return std::nullopt;

        uint64_t value = 0;
        while (std::isdigit(static_cast<unsigned char>(*p))) {
            value = value * 10 + static_cast<uint64_t>(*p - '0');
            if (value > 0xFFFFFF) return std::nullopt;
            ++p;
        }
        // The number has to end the token: "-onlinefix 12abc" is not an AppId.
        if (*p != '\0' && *p != ' ' && *p != '\t' && *p != '"') return std::nullopt;
        if (value == 0) return std::nullopt;
        return static_cast<AppId_t>(value);
    }

    std::unordered_map<AppId_t, std::string> g_GameNameCache;


    // ── SpawnProcess interception ────────────────────────────────────────────
    // CUser_SpawnProcess(pCUser, pExePath, pCommandLine, pWorkingDir,
    //                    pGameID, ...)
    // arg1=pCUser, arg2=pExePath, arg3=pCommandLine, arg4=pWorkingDir
    // arg5=pGameID (CGameID*; low 24 bits = AppId)
    static void OnSpawnProcessHit(OSTPlatform::Trap::Context& ctx, const VehCommon::Int3Site& /*site*/) {
        CGameID* pGameID = VehCommon::GetArg<CGameID*>(ctx, 5);
        if (!pGameID) return;
        AppId_t appId = static_cast<AppId_t>(pGameID->AppID(true));
        const char* cmdLine = VehCommon::GetArg<const char*>(ctx, 3);

        // Resolve the stat SteamID in the background now, so a later
        // GetStatSteamId on a hot path never blocks on HTTP.
        LuaConfig::PrewarmStatSteamId(appId);

        // 每次命中均记录启动参数，用于区分 hook 未触发与启动参数缺失两种情况。
        LOG_MISC_DEBUG("SpawnProcess hit: appid {} cmd=\"{}\"", appId, cmdLine ? cmdLine : "<null>");

        // 只看启动参数，不再要求 appid 在 Lua 名单里：正版玩家加 -onlinefix
        // 联机同样要生效，解锁名单不该决定联机开关。
        const char* onlineFixFlag = cmdLine ? strstr(cmdLine, "-onlinefix") : nullptr;
        if (onlineFixFlag)
        {
            // -onlinefix=<appid> disguises the game as that AppId instead of
            // 480. Some games block 480 or flood its lobby list with other
            // games' rooms; another free or owned AppId avoids both.
            AppId_t fakeAppId = kOnlineFixAppId;
            if (const auto custom = ParseOnlineFixAppId(onlineFixFlag)) {
                if (*custom == appId) {
                    LOG_MISC_WARN("SpawnProcess: -onlinefix={} is the game's own appid, using {}",
                                  *custom, kOnlineFixAppId);
                } else {
                    fakeAppId = *custom;
                }
            }
            g_OnlineFixFakeAppId.store(fakeAppId);
            g_OnlineFixRealAppId = appId;
            g_NetworkingSocketsActive = false;
            // 逐局关闭 appid 翻转：启动期就要读自己 appid 的游戏会卡在黑屏，
            // 这个调用本身分不出游戏意图，只能交给用户按局指定。
            g_SuppressAppIdFlip = strstr(cmdLine, "-realappid") != nullptr;
            pGameID->SetAppID(fakeAppId);
            LOG_MISC_INFO("SpawnProcess: appid {} -> {}, realappid={}, cmd=\"{}\"",
                          appId, fakeAppId, g_SuppressAppIdFlip, cmdLine);
        } else {
            g_OnlineFixRealAppId = 0;
            g_SuppressAppIdFlip = false;
            g_OnlineFixFakeAppId.store(kOnlineFixAppId);
        }
    }

    // ── SteamController_OptedInMask ──────────────────────────────────────────
    // Called by CUser_BuildSpawnEnvBlock with pGameID's appid to
    // compute EnableConfiguratorSupport and the SDL_* env vars.
    // With the fake AppId the spawned game inherits that app's Steam Input
    // opt-in and gameoverlayrenderer hijacks the XInput stream.
    HOOK_FUNC(OptedInMask, int64,void* pThis, AppId_t appId)
    {
        if (g_OnlineFixRealAppId && appId == Hooks_Misc::GetOnlineFixAppId()) {
            LOG_MISC_INFO("OptedInMask: appid {} -> {}",appId, g_OnlineFixRealAppId);
            appId = g_OnlineFixRealAppId;
        }
        return oOptedInMask(pThis, appId);
    }

    // ── CUser_BuildSpawnEnvBlock ─────────────────────────────────────────────
    // pOverlayCGameID drives SteamOverlayGameId, which the in-game
    // overlay reads for screenshot tags, community URLs, and asset
    // selection.  pCGameID drives SteamGameId / SteamAppId; leave it
    // at the fake AppId so the in-game ownership bypass holds.
    HOOK_FUNC(BuildSpawnEnvBlock, int64,
              void* pThis, CGameID* pCGameID, void* a3, void* env,
              CGameID* pOverlayCGameID, void* a6, int a7,
              void* a8, void* a9, unsigned int a10, char a11)
    {
        if (g_OnlineFixRealAppId && pOverlayCGameID
            && pOverlayCGameID->AppID(true) == Hooks_Misc::GetOnlineFixAppId())
        {
            LOG_MISC_INFO("BuildSpawnEnvBlock: SetAppID in OverlayCGameID {} -> {}",
                          pOverlayCGameID->AppID(true), g_OnlineFixRealAppId);
            pOverlayCGameID->SetAppID(g_OnlineFixRealAppId);
        }
        return oBuildSpawnEnvBlock(pThis, pCGameID, a3, env,
                                    pOverlayCGameID, a6, a7,
                                    a8, a9, a10, a11);
    }

    // CAppInfoCache::GetOrAddAppData
    // The injected package keeps Lua-provided ids in PackageInfo::AppIdVec.
    // Some of those ids can actually be depot ids, but we cannot trust the
    // Lua config to classify app ids and depot ids for us. In offline mode,
    // depot ids usually have only placeholder appinfo data. That blocks
    // CClientAppManager_ProcessPendingLicenseUpdates, because it waits for
    // every AppIdVec entry to have resolved appinfo unless the entry has been
    // marked as a known-unknown id by the PICS path. For injected ids that
    // still have placeholder appinfo, set skip_flag so Steam treats them like
    // PICS unknown_appids instead of keeping the license update pending.
    HOOK_FUNC(GetOrAddAppData,CAppData*,void* pCache, AppId_t appId,bool bCreate)
    {
        CAppData* pData = oGetOrAddAppData(pCache, appId, bCreate);
        // LOG_MISC_TRACE("GetOrAddAppData: appId={} bCreate={} -> pData={}", appId, bCreate, pData ? pData->DebugString() : "null");
        // TODO: find a more robust way
        if (LuaConfig::HasDepot(appId, false) && pData && !bCreate && pData->IsUnresolvedAppInfo()) {
            LOG_MISC_DEBUG("GetOrAddAppData: Marking appId {} as skip_flag=true to bypass license update blocking", appId);
            pData->bSkipFlag = true;
        }
        return pData;
    }
}

namespace Hooks_Misc {
    void Install() {
        RESOLVE_C(CUtlBufferEnsureCapacity);

        ARM_CAPTURE_C(GetAppIDForCurrentPipe);
        ARM_CAPTURE_C(GetAppDataFromAppInfo);

        ARM_INT3_C(SpawnProcess, true, &OnSpawnProcessHit, nullptr);

        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildSpawnEnvBlock);
        INSTALL_HOOK_C(OptedInMask);
        // INSTALL_HOOK_C(GetOrAddAppData);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildSpawnEnvBlock);
        UNINSTALL_HOOK(OptedInMask);
        // UNINSTALL_HOOK(GetOrAddAppData);
        UNHOOK_END();
    }

    AppId_t GetAppIDForCurrentPipeWrap() {
        if (!CAPTURE_READY(GetAppIDForCurrentPipe)) {
            LOG_MISC_WARN("GetAppIDForCurrentPipeWrap called before capture — returning 0");
            return 0;
        }
        auto appid = oGetAppIDForCurrentPipe(g_steamEngine);
        if (!appid) {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId=0(Not GamePipe)");
        } else {
            LOG_MISC_TRACE("GetAppIDForCurrentPipeWrap: AppId={}", appid);
        }
        return appid;
    }

    
    AppId_t ResolveAppId() {
        if (g_OnlineFixRealAppId) return g_OnlineFixRealAppId;
        return GetAppIDForCurrentPipeWrap();
    }

    bool IsOnlineFixActive() {
        return g_OnlineFixRealAppId != 0;
    }

    AppId_t GetOnlineFixAppId() {
        return g_OnlineFixFakeAppId.load();
    }

    void NotifyNetworkingSocketsUsed() {
        if (g_OnlineFixRealAppId && !g_NetworkingSocketsActive) {
            g_NetworkingSocketsActive = true;
            if (g_SuppressAppIdFlip)
                LOG_MISC_INFO("NetworkingSockets active: GetAppID keeps the real appid (-realappid)");
            else
                LOG_MISC_INFO("NetworkingSockets active: GetAppID now reports {} for cert match",
                              GetOnlineFixAppId());
        }
    }

    bool ShouldReportOnlineFixAppId() {
        // 翻转的用途是让 P2P socket 的 appid 与 480 会话证书对上，部分游戏靠它联机。
        // 但它一旦触发，本进程余下生命周期里每次询问都答 480；另一些游戏在启动期
        // 问同一句话拿到 480 就会卡死或黑屏。两侧都有游戏需要，而这个调用本身
        // 分不出对方是哪一类，于是用 -realappid 逐局退出。
        if (g_SuppressAppIdFlip) return false;
        return g_OnlineFixRealAppId != 0 && g_NetworkingSocketsActive;
    }
    
    bool EnsureBufferCapacity(CUtlBuffer* pWrite, uint32 newCapacity,bool updatePut)
    {
        if (oCUtlBufferEnsureCapacity) {
            LOG_MISC_DEBUG("Before ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            oCUtlBufferEnsureCapacity(pWrite, newCapacity);
            LOG_MISC_DEBUG("After ensuring CUtlBuffer capacity: {}", pWrite->DebugString());
            if(updatePut) pWrite->m_Put = newCapacity;
            return true;
        }
        LOG_MISC_WARN("EnsureBufferCapacity: oCUtlBufferEnsureCapacity not resolved");
        return false;
    }

    // ── Game name ────────────────────────────────────────────────
    std::string GetGameNameByAppID(AppId_t appId)
    {
        auto it = g_GameNameCache.find(appId);
        if (it != g_GameNameCache.end()) return it->second;

        std::string name;

        if (CAPTURE_READY(GetAppDataFromAppInfo)) {
            char buf[256] = {};
            // "common/name" triggers auto-localization: the function detects
            // prefix "common" (keyType=2) + key "name", then tries
            // "name_localized/<current_lang>" before falling back to "name".
            // Returns strlen+1 on success, -1 on failure.
            int64 len = oGetAppDataFromAppInfo(g_pCAppInfoCache, appId, "common/name",
                reinterpret_cast<uint8*>(buf), sizeof(buf));
            if (len > 1) {
                // Steam may report a length beyond the buffer it was given;
                // clamp so name.assign() cannot read past buf.
                const size_t copyLen = (std::min)(static_cast<size_t>(len - 1), sizeof(buf) - 1);
                name.assign(buf, copyLen);
            }
        }

        LOG_MISC_DEBUG("GetGameNameByAppID({}): {}", appId, name);
        g_GameNameCache[appId] = name;
        return name;
    }

}
