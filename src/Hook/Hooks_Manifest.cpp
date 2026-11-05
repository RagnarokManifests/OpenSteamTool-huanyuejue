#include "Hooks_Manifest.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "dllmain.h"
#include <format>
#include <mutex>
#include <unordered_map>
#include <utility>

// ═══════════════════════════════════════════════════════════════════
//  Manifest override hooks:
//    BuildDepotDependency — patches depot entries' gid/size directly
//      in the output vector (replaces the old KV-tree approach).
// ═══════════════════════════════════════════════════════════════════
namespace {

    // Per-app snapshot of the depot targets we handed Steam while its update
    // was in flight. The first BuildDepotDependency during an update captures
    // what was delivered (override applied); later passes serve the snapshot
    // back, so a pin added or removed mid-download cannot retarget an update
    // that is already running (pausing one mid-flight is what used to leave
    // the app stuck). Cleared as soon as a pass runs with no update active,
    // where the live values apply again.
    // appId -> depotId -> (gid, size).
    std::mutex g_updateTargetMutex;
    std::unordered_map<AppId_t,
        std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>> g_updateTargets;

    std::string DepotEntryDebug(const DepotEntry& e) {
        return std::format("DepotId={} AppId={} Gid={} Size={} Dlc={} Lcs={} Carry={} Shared={}",
            e.DepotId, e.AppId, e.ManifestGid, e.ManifestSize, e.DlcAppId,
            (int)e.LcsRequired, (int)e.bNotNewTarget, (int)e.SharedInstall);
    }

    HOOK_FUNC(BuildDepotDependency, bool, void* pUserAppMgr, AppId_t AppId,
              void* pUserConfig, CUtlVector<DepotEntry>* pDepotInfo,
              CUtlVector<DepotEntry>* pSharedDepotInfo, void* pSteamApp,
              uint32* pBuildId, bool* pbBetaFallback)
    {
        bool result = oBuildDepotDependency(pUserAppMgr, AppId, pUserConfig,
            pDepotInfo, pSharedDepotInfo, pSteamApp, pBuildId, pbBetaFallback);

        LOG_MANIFEST_TRACE("BuildDepotDependency: AppId={} pUserConfig=0x{:X} result={} pSteamApp=0x{:X} pBuildId={} pbBetaFallback={}",
            AppId, (uintptr_t)pUserConfig, result, (uintptr_t)pSteamApp,
            pBuildId ? *pBuildId : 0, pbBetaFallback ? *pbBetaFallback : false);
        if (pDepotInfo) {
            LOG_MANIFEST_TRACE("pDepotInfo->nCount={}", pDepotInfo->m_Size);
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  [{}] {}", i, DepotEntryDebug(pDepotInfo->m_Memory.m_pMemory[i]));
            }
        }
        if (pSharedDepotInfo) {
            LOG_MANIFEST_TRACE("pSharedDepotInfo->nCount={}", pSharedDepotInfo->m_Size);
            for (uint32 i = 0; i < pSharedDepotInfo->m_Size; ++i) {
                LOG_MANIFEST_TRACE("  shared[{}] {}", i, DepotEntryDebug(pSharedDepotInfo->m_Memory.m_pMemory[i]));
            }
        }

        if (!result) return result;

        const auto& overrides = LuaConfig::GetManifestOverrides();

        // Snapshot the targets on the first pass with an update in flight, and
        // drop it once a pass runs with no update active. The lock is held for
        // the whole pass below: `held` points into g_updateTargets. Taken for
        // any app, not only pinned ones: removing the last pin mid-update
        // must still serve the target the update started with.
        std::lock_guard<std::mutex> targetLock(g_updateTargetMutex);
        const bool updating = Hooks_SteamUI::IsAppUpdating(AppId);
        std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>* held = nullptr;
        {
            auto it = g_updateTargets.find(AppId);
            if (updating) {
                if (it == g_updateTargets.end())
                    it = g_updateTargets.emplace(AppId,
                        std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>{}).first;
                held = &it->second;
            } else if (it != g_updateTargets.end()) {
                g_updateTargets.erase(it);
            }
        }
        if (overrides.empty() && !held) return result;

        if (pDepotInfo && pDepotInfo->m_Size) {
            for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];

                // Update in flight: keep the exact target the update started
                // with, whatever the overrides say now.
                if (held) {
                    auto h = held->find(e.DepotId);
                    if (h != held->end()) {
                        if (e.ManifestGid != h->second.first || e.ManifestSize != h->second.second) {
                            LOG_MANIFEST_INFO("BuildDepotDependency: app {} updating, holding depot {} "
                                "at gid={} size={} (current target gid={} size={})",
                                AppId, e.DepotId, h->second.first, h->second.second,
                                e.ManifestGid, e.ManifestSize);
                        }
                        e.ManifestGid  = h->second.first;
                        e.ManifestSize = h->second.second;
                        continue;
                    }
                }

                auto it = overrides.find(e.DepotId);
                if (it != overrides.end()) {
                    // if size=0 in the override, keep the original size(affects download display but not the actual download)
                    uint64_t newSize = it->second.size ? it->second.size : e.ManifestSize;
                    LOG_MANIFEST_INFO("BuildDepotDependency: patching depot {} gid={}->{} size={}->{}",
                        e.DepotId, e.ManifestGid, it->second.gid,
                        e.ManifestSize, newSize);
                    e.ManifestGid  = it->second.gid;
                    e.ManifestSize = newSize;
                }
            }

            // Capture what we just delivered as this update's target set.
            if (held) {
                for (uint32 i = 0; i < pDepotInfo->m_Size; ++i) {
                    const DepotEntry& e = pDepotInfo->m_Memory.m_pMemory[i];
                    (*held)[e.DepotId] = {e.ManifestGid, e.ManifestSize};
                }
            }
        }
        return result;
    }

} // anonymous namespace

namespace Hooks_Manifest {

    void Install() {
        HOOK_BEGIN();
        INSTALL_HOOK_C(BuildDepotDependency);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK(BuildDepotDependency);
        UNHOOK_END();
    }
}
