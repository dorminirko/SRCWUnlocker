#include "Features.h"

// =============================================================================
// Steam Achievement Unlocking
// =============================================================================
typedef void* (*SteamUserStats_t)();
typedef bool  (*RequestCurrentStats_t)(void*);
typedef uint32_t (*GetNumAchievements_t)(void*);
typedef const char* (*GetAchievementName_t)(void*, uint32_t);
typedef bool  (*SetAchievement_t)(void*, const char*);
typedef bool  (*StoreStats_t)(void*);

bool UnlockSteamAchievements()
{
    HMODULE steamApi = GetModuleHandleA("steam_api64.dll");
    if (!steamApi) return false;
    auto fnGetStats = (SteamUserStats_t)GetProcAddress(steamApi, "SteamAPI_SteamUserStats_v012");
    if (!fnGetStats) fnGetStats = (SteamUserStats_t)GetProcAddress(steamApi, "SteamAPI_SteamUserStats_v011");
    if (!fnGetStats) fnGetStats = (SteamUserStats_t)GetProcAddress(steamApi, "SteamUserStats");
    if (!fnGetStats) return false;
    void* userStats = fnGetStats();
    if (!userStats) return false;
    auto fnReq = (RequestCurrentStats_t)GetProcAddress(steamApi, "SteamAPI_ISteamUserStats_RequestCurrentStats");
    auto fnNum = (GetNumAchievements_t)GetProcAddress(steamApi, "SteamAPI_ISteamUserStats_GetNumAchievements");
    auto fnNam = (GetAchievementName_t)GetProcAddress(steamApi, "SteamAPI_ISteamUserStats_GetAchievementName");
    auto fnSet = (SetAchievement_t)GetProcAddress(steamApi, "SteamAPI_ISteamUserStats_SetAchievement");
    auto fnSto = (StoreStats_t)GetProcAddress(steamApi, "SteamAPI_ISteamUserStats_StoreStats");
    if (!fnReq || !fnNum || !fnNam || !fnSet || !fnSto) return false;
    fnReq(userStats); Sleep(500);
    uint32_t n = fnNum(userStats); if (n == 0) return false;
    int ok = 0;
    for (uint32_t i = 0; i < n; i++) { const char* a = fnNam(userStats, i); if (a && a[0] && fnSet(userStats, a)) ok++; }
    fnSto(userStats);
    std::cout << "[Achievements] " << ok << "/" << n << "\n";
    return true;
}

void Cleanup() { if (cfg.Console) FreeConsole(); }

// =============================================================================
// Super Sonic — Native ExecFunction pointer swap
// =============================================================================

using FNativeFuncPtr = void (*)(void* Context, void* TheStack, void* Result);

static constexpr uint8_t kSuperSonicDriverId = 46;
static constexpr int32_t kSSCharaSelectIndex = 46;

static FNativeFuncPtr s_origExecIsDriverSelectable = nullptr;
static FNativeFuncPtr s_origExecGetCSIndex         = nullptr;

static SDK::UFunction* s_ufnIsDriverSelectable = nullptr;
static SDK::UFunction* s_ufnGetCSIndex         = nullptr;

static bool s_ssHooksInstalled = false;
static bool s_ssLoggedOnce = false;

void ExecIsDriverSelectableHook(void* Context, void* TheStack, void* Result)
{
    s_origExecIsDriverSelectable(Context, TheStack, Result);
    if (Result) {
        uint8_t* retPtr = static_cast<uint8_t*>(Result);
        uint8_t driverId = *(retPtr - 1);
        if (driverId == kSuperSonicDriverId) {
            *retPtr = 1;
            if (!s_ssLoggedOnce) std::cout << "[SS] ExecIsDriverSelectable: driver 46 -> true\n";
        }
    }
}

void ExecGetCSIndexHook(void* Context, void* TheStack, void* Result)
{
    s_origExecGetCSIndex(Context, TheStack, Result);
    if (Result) {
        int32_t* retPtr = static_cast<int32_t*>(Result);
        uint8_t driverId = *(reinterpret_cast<uint8_t*>(Result) - 4);
        if (driverId == kSuperSonicDriverId && *retPtr < 0) {
            *retPtr = kSSCharaSelectIndex;
            if (!s_ssLoggedOnce) {
                std::cout << "[SS] ExecGetCSIndex: driver 46 -> " << kSSCharaSelectIndex << "\n";
                s_ssLoggedOnce = true;
            }
        }
    }
}

static void InstallSuperSonicHooks()
{
    if (s_ssHooksInstalled) return;
    s_ufnIsDriverSelectable = Reflect::FindFunction("CharaSelectUtilityLibrary", "IsDriverSelectable");
    s_ufnGetCSIndex = Reflect::FindFunction("DriverDataUtilityLibrary", "GetCharaSelectIndexByDriverId");
    if (!s_ufnIsDriverSelectable) { std::cout << "[SS] ERROR: IsDriverSelectable not found\n"; return; }
    if (!s_ufnGetCSIndex) { std::cout << "[SS] ERROR: GetCharaSelectIndexByDriverId not found\n"; return; }

    FNativeFuncPtr* pExecIDS = reinterpret_cast<FNativeFuncPtr*>(reinterpret_cast<uint8_t*>(s_ufnIsDriverSelectable) + 0xD8);
    FNativeFuncPtr* pExecGCS = reinterpret_cast<FNativeFuncPtr*>(reinterpret_cast<uint8_t*>(s_ufnGetCSIndex) + 0xD8);
    s_origExecIsDriverSelectable = *pExecIDS;
    s_origExecGetCSIndex = *pExecGCS;

    std::cout << "[SS] IsDriverSelectable ExecFunc @ 0x" << std::hex << reinterpret_cast<uintptr_t>(s_origExecIsDriverSelectable) << std::dec << "\n";
    std::cout << "[SS] GetCSIndex ExecFunc @ 0x" << std::hex << reinterpret_cast<uintptr_t>(s_origExecGetCSIndex) << std::dec << "\n";

    if (!s_origExecIsDriverSelectable || !s_origExecGetCSIndex) {
        std::cout << "[SS] ERROR: ExecFunction is null\n"; return;
    }

    DWORD oldProtect;
    VirtualProtect(pExecIDS, sizeof(FNativeFuncPtr), PAGE_READWRITE, &oldProtect);
    *pExecIDS = &ExecIsDriverSelectableHook;
    VirtualProtect(pExecIDS, sizeof(FNativeFuncPtr), oldProtect, &oldProtect);
    VirtualProtect(pExecGCS, sizeof(FNativeFuncPtr), PAGE_READWRITE, &oldProtect);
    *pExecGCS = &ExecGetCSIndexHook;
    VirtualProtect(pExecGCS, sizeof(FNativeFuncPtr), oldProtect, &oldProtect);
    s_ssHooksInstalled = true;
    std::cout << "[SS] ExecFunction swaps installed!\n";
}

// =============================================================================
// Hook & Clear
// =============================================================================
void HookGame()
{
    // LoadConfig() is already called in dllmain.cpp Init()
    // No need to call again here

    bool bHooked = false;
    while (!bHooked) {
        auto World = SDK::UWorld::GetWorld();
        auto Engine = SDK::UEngine::GetEngine();
        if (!World || !Engine || !Engine->GameViewport) continue;
        Orig_AActor_ProcessEvent = reinterpret_cast<AActor_ProcessEvent_t>(
            PatternScan("48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC ? F7 82 B0 00 00 00 ? ? ? ?"));
        if (!Orig_AActor_ProcessEvent) continue;
        Orig_AActor_ProcessEvent = reinterpret_cast<AActor_ProcessEvent_t>(
            TrampHook64((BYTE*)Orig_AActor_ProcessEvent, (BYTE*)hk_AActor_ProcessEvent, 15));
        bHooked = true;
        if (!cfg.HotkeyEnabled) std::cout << "[SRCW] Hooked! AutoRun mode.\n";
        else std::cout << "[SRCW] Hooked! Hotkey 0x" << std::hex << cfg.UnlockKey << std::dec << "\n";
    }
}

void Clear()
{
    auto* contentData = Reflect::FindInstance("ContentDataAsset");
    auto* contentMgr = Reflect::FindInstance("UnionContentManager");
    if (!contentData || !contentMgr) { bCleared = true; return; }

    SDK::FProperty* pkgListProp = Reflect::FindProperty(contentData->Class, "PackageDataList");
    if (!pkgListProp) { bCleared = true; return; }
    auto* arrProp = static_cast<SDK::FArrayProperty*>(pkgListProp);
    auto* innerProp = arrProp->InnerProperty;
    auto* structProp = static_cast<SDK::FStructProperty*>(innerProp);
    int32_t pidOffset = Reflect::FindPropertyOffset(structProp->Struct, "P_Id");
    int32_t elemSize = innerProp->ElementSize;
    if (pidOffset < 0 || elemSize <= 0) { bCleared = true; return; }

    void* pkgListAddr = reinterpret_cast<uint8_t*>(contentData) + pkgListProp->Offset;
    int32_t pkgCount = ReflectRaw::TArrayNum(pkgListAddr);
    std::vector<int32_t> allPkgIds;
    for (int i = 0; i < pkgCount; i++) {
        void* elem = ReflectRaw::TArrayAt(pkgListAddr, i, elemSize);
        allPkgIds.push_back(ReflectRaw::ReadInt32(elem, pidOffset));
    }

    SDK::FProperty* mPkgProp = Reflect::FindProperty(contentMgr->Class, "m_packageIds");
    if (!mPkgProp || allPkgIds.empty()) { bCleared = true; return; }
    void* mPkgAddr = reinterpret_cast<uint8_t*>(contentMgr) + mPkgProp->Offset;
    auto* mPkgArr = reinterpret_cast<ReflectRaw::RawTArray*>(mPkgAddr);

    std::vector<int32_t> existing;
    for (int32_t i = 0; i < mPkgArr->NumElements; i++)
        existing.push_back(reinterpret_cast<int32_t*>(mPkgArr->Data)[i]);

    int32_t added = 0;
    for (auto pid : allPkgIds) {
        bool found = false;
        for (auto eid : existing) if (eid == pid) { found = true; break; }
        if (!found) { existing.push_back(pid); added++; }
    }

    if (added > 0) {
        int32_t nc = (int32_t)existing.size();
        if (nc > mPkgArr->MaxElements) {
            int32_t* nd = (int32_t*)VirtualAlloc(nullptr, nc * sizeof(int32_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (nd) { memcpy(nd, existing.data(), nc * sizeof(int32_t)); mPkgArr->Data = nd; mPkgArr->NumElements = nc; mPkgArr->MaxElements = nc; }
        } else {
            for (int32_t j = mPkgArr->NumElements; j < nc; j++)
                reinterpret_cast<int32_t*>(mPkgArr->Data)[j] = existing[j];
            mPkgArr->NumElements = nc;
        }
        Reflect::CallStaticBool("UnionContentUtils", "RequestCheckContent", true);
        std::cout << "[Clear] Injected " << added << " package IDs\n";
    }
    bCleared = true;
}

// =============================================================================
// Phased unlock
// =============================================================================
bool RunUnlockPhase(int phase)
{
    switch (phase) {
    case 0:  { if (cfg.HonorTitles) { for (int i = 0; i < 500; i++) Reflect::CallStaticInt32("AppSaveGameHelper", "UnlockHonorTitle", i); std::cout << "[Phase 0] Honor titles\n"; } return true; }
    case 1:  { if (cfg.Drivers) { int n = Reflect::GetEnumNum("EDriverId"); for (int i = 0; i < n; i++) { Reflect::CallStaticUInt8("AppSaveGameHelper", "SetDriverSelectable", (uint8_t)i); Reflect::CallStaticUInt8("AppSaveGameHelper", "ClearDriverNew", (uint8_t)i); } std::cout << "[Phase 1] Drivers (" << n << ")\n"; } return true; }
    case 2:  { if (cfg.MachineCustomize) { Reflect::CallStatic("MachineCustomizeUtilityLibrary", "StoreAllAura"); Reflect::CallStatic("MachineCustomizeUtilityLibrary", "StoreAllHorn"); Reflect::CallStatic("MachineCustomizeUtilityLibrary", "StoreAllMachineAssembly"); Reflect::CallStatic("MachineCustomizeUtilityLibrary", "StoreAllMachineParts"); Reflect::CallStatic("MachineCustomizeUtilityLibrary", "StoreAllSticker"); Reflect::CallStatic("MachineCustomizeUtilityLibrary", "UnlockGadgetAll"); std::cout << "[Phase 2] Machine customize\n"; } return true; }
    case 3:  { if (cfg.ColorPresets) { int n = Reflect::GetEnumNum("EMachineId"); for (int i = 0; i < n; i++) Reflect::CallStaticUInt8("MachineCustomizeUtilityLibrary", "UnlockMachinePresetColor", (uint8_t)i); std::cout << "[Phase 3] Color presets\n"; } return true; }
    case 4:  { if (cfg.MirrorSpeed) { Reflect::CallStaticBool("AppSaveGameHelper", "SetOpenMirror", true); Reflect::CallStaticBool("AppSaveGameHelper", "SetOpenSuperSonicSpeed", true); std::cout << "[Phase 4] Mirror & SSS\n"; } return true; }
    case 5:  { if (cfg.Music) { for (int i = 0; i < 200; i++) Reflect::CallStaticInt32("AppSaveGameHelper", "SetAlbumAvailable", i); for (int i = 0; i < 500; i++) Reflect::CallStaticInt32("AppSaveGameHelper", "SetTrackAvailable", i); std::cout << "[Phase 5] Music\n"; } return true; }
    case 6:  { if (cfg.StagesDLC) { Reflect::CallStaticBool("UnionContentUtils", "RequestCheckContent", true); std::cout << "[Phase 6] DLC stages\n"; } return true; }
    case 7:  {
        if (cfg.StagesGPOpen) {
            auto* gp = Reflect::FindCDO("CheatGrandPrix");
            if (gp) { Reflect::CallInstanceBool(gp, "CheatGrandPrix", "SetGrandPrixOpenedAll", true); Reflect::CallInstanceBool(gp, "CheatGrandPrix", "SetSetGrandPrixLeast1Play", true); }
            int n = Reflect::GetEnumNum("EGrandPrixId"); for (int i = 1; i < n; i++) Reflect::CallStaticUInt8Bool("AppSaveGameHelper", "SetGrandPrixLeast1PlayEachGrandPrix", (uint8_t)i, true);
            int ne = Reflect::GetEnumNum("EGrandPrixEventFlag"); for (int i = 0; i < ne; i++) Reflect::CallStaticUInt8Bool("AppSaveGameHelper", "SetDodonpaEventCompleteFlag", (uint8_t)i, true);
            Reflect::CallStaticBool("AppSaveGameHelper", "SetGrandPrixLeast1Play", true);
            std::cout << "[Phase 7] GP opened\n"; }
        return true; }
    case 8:  {
        if (cfg.StagesSecret) {
            auto* gp = Reflect::FindCDO("CheatGrandPrix");
            if (gp) { Reflect::CallInstance(gp, "CheatGrandPrix", "SetPlayedAnotherStageAll"); Reflect::CallInstanceUInt8Bool(gp, "CheatGrandPrix", "SetClearedGrandPrixEnding", 0, true); Reflect::CallInstanceUInt8Bool(gp, "CheatGrandPrix", "SetClearedGrandPrixEnding", 1, true); }
            std::cout << "[Phase 8] Secret stages\n"; }
        return true; }
    case 9:  { if (cfg.GadgetPlate) { Reflect::CallStaticUInt8("MachineCustomizeUtilityLibrary", "SetCurrentGadgetPlateIdUseId", 7); Reflect::CallStatic("MachineCustomizeUtilityLibrary", "UpdateGadgetSlotNumInUserData"); std::cout << "[Phase 9] Gadget plate\n"; } return true; }
    case 10: { if (cfg.Challenges) { Reflect::CallStatic("CheatChallenge", "AllChallengeClear"); Reflect::CallStaticBool("AppSaveGameHelper", "SetCompleteMainChallenge", true); Reflect::CallStaticBool("AppSaveGameHelper", "SetCompleteSpecialChallenge", true); int32_t pc = Reflect::CallStaticRetInt32("ChallengeStatsUtility", "GetChallengeProgressCount"); Reflect::CallStaticInt32("AppSaveGameHelper", "SetChallengeShowProgress", pc); Reflect::CallStaticFloat("AppSaveGameHelper", "SetChallengeLastShowProgress", 1.0f); std::cout << "[Phase 10] Challenges\n"; } return true; }

    case 11: {
        if (cfg.MirrorSpeed) {
            Reflect::CallStaticUInt8("AppSaveGameHelper", "SetDriverSelectable", kSuperSonicDriverId);
            Reflect::CallStaticUInt8("AppSaveGameHelper", "ClearDriverNew", kSuperSonicDriverId);
            Reflect::CallStaticBool("AppSaveGameHelper", "SetOpenSuperSonicSpeed", true);
            std::cout << "[Phase 11] Super Sonic save-data set\n";
        }
        return true; }

    case 12: { if (cfg.NF_CompleteMachine) Reflect::CallStatic("MachineCustomizeUtilityLibrary", "DisableCompleteMachineNewFlags"); if (cfg.NF_Sticker) Reflect::CallStatic("MachineCustomizeUtilityLibrary", "DisableStickerNewFlags"); if (cfg.NF_ColorPreset) Reflect::CallStatic("MachineCustomizeUtilityLibrary", "DisableMachineColorPresetNewFlags"); if (cfg.NF_Gadget) Reflect::CallStatic("MachineCustomizeUtilityLibrary", "DisableDisplayedGadgetNewFlags"); std::cout << "[Phase 12] Machine NF\n"; return true; }
    case 13: { if (cfg.NF_PartsSpeed)  Reflect::CallStaticUInt8("MachineCustomizeUtilityLibrary", "DisablePartsListNewFlagByType", 0); return true; }
    case 14: { if (cfg.NF_PartsAccel)  Reflect::CallStaticUInt8("MachineCustomizeUtilityLibrary", "DisablePartsListNewFlagByType", 1); return true; }
    case 15: { if (cfg.NF_PartsHandle) Reflect::CallStaticUInt8("MachineCustomizeUtilityLibrary", "DisablePartsListNewFlagByType", 2); return true; }
    case 16: { if (cfg.NF_PartsPower)  Reflect::CallStaticUInt8("MachineCustomizeUtilityLibrary", "DisablePartsListNewFlagByType", 3); return true; }
    case 17: { if (cfg.NF_PartsDash)   Reflect::CallStaticUInt8("MachineCustomizeUtilityLibrary", "DisablePartsListNewFlagByType", 4); return true; }
    case 18: { if (cfg.NF_Horn) { int n = Reflect::GetEnumNum("EMachineHornType"); for (int i = 0; i < n; i++) Reflect::CallStaticUInt8Bool("MachineCustomizeUtilityLibrary", "SetCustomMachineHornNew", (uint8_t)i, false); } return true; }
    case 19: { if (cfg.NF_HonorTitles) { for (int i = 0; i < 500; i++) Reflect::CallStaticInt32("AppSaveGameHelper", "ResetNewHonorTitle", i); } return true; }
    case 20: { if (cfg.NF_Jukebox) { /* Add jukebox NF logic if available */ } return true; }
    case 21: { if (cfg.NF_Challenges) { int32_t pc = Reflect::CallStaticRetInt32("ChallengeStatsUtility", "GetChallengeProgressCount"); Reflect::CallStaticInt32("AppSaveGameHelper", "SetChallengeShowProgress", pc); Reflect::CallStaticFloat("AppSaveGameHelper", "SetChallengeLastShowProgress", 1.0f); } if (cfg.NF_Rewards) Reflect::CallStatic("AppSaveGameHelper", "ClearRewardGetDisplayRequestDataAll"); return true; }
    case 22: { if (cfg.Achievements) { UnlockSteamAchievements(); std::cout << "[Phase 22] Achievements\n"; } return true; }
    default: return false;
    }
}

// =============================================================================
// ProcessEvent Hook
// =============================================================================
void __fastcall hk_AActor_ProcessEvent(SDK::AActor* Class, SDK::UFunction* Function, void* Parms)
{
    // Install Super Sonic ExecFunction hooks on first call
    if (!s_ssHooksInstalled) {
        InstallSuperSonicHooks();
    }

    // DLC Clear
    if (!bCleared)
        Clear();

    // AutoRun (default behavior - no hotkey needed)
    if (!cfg.HotkeyEnabled && !bUnlockStarted && !bUnlockDone) {
        if (!bAutoRunReady) { 
            if (!Function->GetName().compare("OnInitStateSelectPlayMode")) 
                bAutoRunReady = true; 
        }
        else { 
            bUnlockStarted = true; 
            unlockPhase = 0; 
            lastPhaseTime = GetTickCount64(); 
            std::cout << "[SRCW] AutoRun starting...\n"; 
        }
    }

    // Hotkey mode
    if (cfg.HotkeyEnabled && !bUnlockStarted && !bUnlockDone) {
        bool kd = (GetAsyncKeyState(cfg.UnlockKey) & 0x8000) != 0;
        if (kd && !bKeyWasDown) { 
            bUnlockStarted = true; 
            unlockPhase = 0; 
            lastPhaseTime = GetTickCount64(); 
            std::cout << "[SRCW] Hotkey pressed...\n"; 
        }
        bKeyWasDown = kd;
    }

    // Phased unlock timer
    if (bUnlockStarted && !bUnlockDone) {
        ULONGLONG now = GetTickCount64();
        if (now - lastPhaseTime >= (ULONGLONG)cfg.PhaseDelayMs) {
            if (RunUnlockPhase(unlockPhase)) { 
                unlockPhase++; 
                lastPhaseTime = now; 
            }
            else { 
                bUnlockDone = true; 
                std::cout << "All phases complete!\n"; 
            }
        }
    }

    // ===== CALL ORIGINAL =====
    Orig_AActor_ProcessEvent(Class, Function, Parms);
}
