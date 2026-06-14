// foxhole_esp.dll  –  Simplified ImGui ESP for Foxhole
// Inject into Foxhole-Win64-Shipping.exe
//
// Features:
//   - Player health bars with team color differentiation
//   - Vehicle health tracking
//   - Structure/Building health tracking
//   - Lines connecting local player to enemy players only
//   - Local player health display
//   - Vehicle proximity detection every 100ms
//   - Player skeleton bones rendering (simplified humanoid structure)
//   - Vehicle class auto-caching every 5 seconds
//   - AUTOAIM with bone targeting (Head/Central)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>

// ImGui
#include "../DeepRock/imgui/imgui.h"
#include "../DeepRock/imgui/backends/imgui_impl_win32.h"
#include "../DeepRock/imgui/backends/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#include "4.24.3-0+++UE4+Release-4.24-War/CppSDK/SDK.hpp"

// ═══════════════════════════════════════════════════════════════
// UWorld::GetWorld() implementation
// ═══════════════════════════════════════════════════════════════
namespace SDK {
    UWorld* UWorld::GetWorld()
    {
        return *reinterpret_cast<UWorld**>(
            InSDKUtils::GetImageBase() + Offsets::GWorld);
    }

    FName UKismetStringLibrary::Conv_StringToName(const UC::FString&)
    {
        return {};
    }

    bool APlayerController::ProjectWorldLocationToScreen(
        const FVector& WorldLocation, FVector2D* ScreenLocation,
        bool bPlayerViewportRelative) const
    {
        static UFunction* fn = nullptr;
        if (!fn)
            fn = Class->GetFunction("PlayerController", "ProjectWorldLocationToScreen");
        if (!fn) return false;

        struct Params {
            FVector   WorldLocation;
            FVector2D ScreenLocation;
            bool      bPlayerViewportRelative;
            bool      ReturnValue;
        } p{};
        p.WorldLocation = WorldLocation;
        p.bPlayerViewportRelative = bPlayerViewportRelative;

        auto savedFlags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;
        ProcessEvent(fn, &p);
        fn->FunctionFlags = savedFlags;

        if (ScreenLocation) *ScreenLocation = p.ScreenLocation;
        return p.ReturnValue;
    }
}

// ═══════════════════════════════════════════════════════════════
// Helpers
// ═══════════════════════════════════════════════════════════════
static inline bool IsValidPtr(uintptr_t p)
{
    return p > 0x10000ULL && p < 0x800000000000ULL;
}

struct FVec3 { float x, y, z; };

struct BoneData {
    FVec3 pos;
    bool  valid;
};

struct PlayerESP {
    FVec3                pos{};
    float                hp = 0.f;
    int                  maxHP = 100;
    uint8_t              team = 255;
    bool                 isLocal = false;
    float                dist = 0.f;
    uintptr_t            actorAddr = 0;
    FVec3                prevPos{};
    bool                 hasPrev = false;
    float                viewYaw = 0.f;
    std::vector<BoneData> bones;
};

struct VehicleESP {
    FVec3     pos{};
    float     dist = 0.f;
    int       health = 0;
    uint8_t   teamId = 255;
    char      clsName[64]{};
};

struct StructureESP {
    FVec3     pos{};
    float     dist = 0.f;
    int       health = 0;
    int       maxHealth = 0;
    uint8_t   teamId = 255;
    char      clsName[64]{};
};

struct NearbyVehicle {
    float     dist = 0.f;
    int       health = 0;
    uint8_t   teamId = 255;
    char      clsName[64]{};
    FVec3     pos{};
};

// ═══════════════════════════════════════════════════════════════
// Global state
// ═══════════════════════════════════════════════════════════════
namespace G
{
    uintptr_t              base = 0;
    std::vector<PlayerESP> players;
    std::vector<PlayerESP> playersNoFilter;
    std::mutex             mtx;

    uintptr_t              localPawn = 0;
    uintptr_t              charClass = 0;
    int                    localTeam = -1;
    std::atomic<uintptr_t> localPCAddr{ 0 };
    std::atomic<uint32_t>  lastScanTick{ 0 };

    // ESP settings
    bool   espEnabled = true;
    bool   showMenu = false;
    bool   enemyOnly = false;
    bool   showHPBar = true;
    bool   showDist = true;
    float  maxDist = 200000.f;
    bool   showLines = true;
    bool   showBones = true;
    ImU32  boneColor = IM_COL32(100, 200, 255, 200);

    // Vehicle ESP
    bool                        showVehicleESP = false;
    bool                        vehicleEnemyOnly = false;
    float                       vehicleMaxDist = 500.f;
    std::vector<VehicleESP>     vehicles;
    std::vector<uintptr_t>      vehicleClasses;
    bool                        vehicleClassesDirty = true;
    std::atomic<uint32_t>       lastVehicleClassScan{ 0 };

    // Structure ESP
    bool                        showStructureESP = false;
    bool                        structureEnemyOnly = false;
    float                       structureMaxDist = 300.f;
    std::vector<StructureESP>   structures;
    std::vector<uintptr_t>      structureClasses;
    bool                        structureClassesDirty = true;

    // Vehicle proximity detection
    bool                        enableVehicleProximity = true;
    float                       vehicleProximityDist = 1000.f;
    std::vector<NearbyVehicle>  nearbyVehicles;
    std::atomic<uint32_t>       lastVehicleProximityScan{ 0 };

    // Autoaim settings
    bool                        enableAutoaim = false;
    int                         autoaimBoneTarget = 0;  // 0 = head, 1 = central (torso)
    bool                        autoaimEnemyOnly = true;
    float                       autoaimSmoothing = 0.3f;  // 0.0 = instant, 1.0 = very smooth
    uint8_t                     autoaimActivationKey = VK_RBUTTON;  // Right mouse button
    bool                        autoaimActive = false;  // Whether targeting is currently active
    uintptr_t                   autoaimTargetAddr = 0;  // Current target actor address
    FVec3                       autoaimTargetScreenPos{};  // Screen position of target
}

// ─────────────────────────────────────────────────────────
// Get actor world position
// ─────────────────────────────────────────────────────────
static bool GetWorldPos(uintptr_t actorAddr, FVec3& out)
{
    if (!IsValidPtr(actorAddr)) return false;
    auto* a = reinterpret_cast<SDK::AActor*>(actorAddr);
    auto* root = a->RootComponent;
    if (!root || !IsValidPtr(reinterpret_cast<uintptr_t>(root))) return false;
    const SDK::FVector& loc = root->RelativeLocation;
    out = { loc.X, loc.Y, loc.Z };
    return true;
}

// ─────────────────────────────────────────────────────────
// Get skeleton bones - ESTRUCTURA ESQUELÉTICA SIMPLIFICADA
// (Sin manos, codos ni hombros - Solo cabeza, torso, piernas)
// ─────────────────────────────────────────────────────────
static bool GetSkeletonBones(uintptr_t actorAddr, std::vector<BoneData>& bones)
{
    if (!IsValidPtr(actorAddr)) return false;

    bones.clear();
    bones.reserve(7);

    __try {
        FVec3 basePos{};
        if (!GetWorldPos(actorAddr, basePos)) return false;

        // Estructura esquelética simplificada: 7 puntos
        // Índices:
        // 0: Cabeza
        // 1: Torso (centro)
        // 2: Entrepierna
        // 3-4: Rodillas (izq, der)
        // 5-6: Pies (izq, der)

        float offsets[][3] = {
            // 0: Cabeza
            {0.f,  0.f, 100.f},
            // 1: Torso (centro)
            {0.f, 0.f, 50.f},
            // 2: Entrepierna
            {0.f, 0.f, -5.f},
            // 3: Rodilla izquierda
            {-25.f, 0.f, -45.f},
            // 4: Rodilla derecha
            {25.f, 0.f, -45.f},
            // 5: Pie izquierdo
            {-25.f, 0.f, -85.f},
            // 6: Pie derecho
            {25.f, 0.f, -85.f},
        };

        for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
            BoneData bone;
            bone.pos.x = basePos.x + offsets[i][0];
            bone.pos.y = basePos.y + offsets[i][1];
            bone.pos.z = basePos.z + offsets[i][2];
            bone.valid = true;
            bones.push_back(bone);
        }

        return bones.size() > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ─────────────────────────────────────────────────────────
// World → Screen projection
// ─────────────────────────────────────────────────────────
static bool W2S(const FVec3& world, float& sx, float& sy)
{
    auto pcAddr = G::localPCAddr.load(std::memory_order_acquire);
    if (!IsValidPtr(pcAddr)) return false;
    auto* pc = reinterpret_cast<SDK::APlayerController*>(pcAddr);

    SDK::FVector   wl{ world.x, world.y, world.z };
    SDK::FVector2D sl{};
    if (!pc->ProjectWorldLocationToScreen(wl, &sl, false)) return false;

    sx = sl.X;
    sy = sl.Y;
    return true;
}

static bool SafeW2S(const FVec3& world, float& sx, float& sy)
{
    float tx = 0.f, ty = 0.f;
    bool ok = false;
    __try {
        ok = W2S(world, tx, ty);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    sx = tx; sy = ty;
    return ok;
}


// ─────────────────────────────────────────────────────────
// Helper para dibujar conexión entre dos bones
// ─────────────────────────────────────────────────────────
static void DrawBoneConnection(ImDrawList* dl, const std::vector<BoneData>& bones,
    size_t fromIdx, size_t toIdx, ImU32 color)
{
    if (fromIdx >= bones.size() || toIdx >= bones.size()) return;
    if (!bones[fromIdx].valid || !bones[toIdx].valid) return;

    float x1, y1, x2, y2;
    if (SafeW2S(bones[fromIdx].pos, x1, y1) && SafeW2S(bones[toIdx].pos, x2, y2)) {
        dl->AddLine({ x1, y1 }, { x2, y2 }, color, 1.5f);
    }
}


static float Dist3D(const FVec3& a, const FVec3& b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// ═══════════════════════════════════════════════════════════════
// Helper: Invert team (0→1, 1→0)
// ═══════════════════════════════════════════════════════════════
static inline uint8_t InvertTeam(uint8_t team)
{
    if (team == 0) return 1;
    if (team == 1) return 0;
    return 255;
}

// ═══════════════════════════════════════════════════════════════
// Helper: Convert string to lowercase
// ═══════════════════════════════════════════════════════════════
static std::string ToLower(const std::string& str)
{
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

// ═══════════════════════════════════════════════════════════════
// Vehicle keyword checking - Lista basada
// ═══════════════════════════════════════════════════════════════
static const std::vector<std::string> VEHICLE_KEYWORDS = {
    // TANQUES
    "devitt", "silverhand", "gallagher", "noble", "flood", "tank", "tanks",
    // CAÑONES AUTOPROPULSADOS (SPG)
    "lance", "cullen", "ares", "falchion", "spatha", "talos", "bardiche", "ranseur",
    // TANQUES BLINDADOS (ARMORED)
    "o'brien", "king spire", "king gallant", "niska", "armored",
    // TANQUES LIGEROS (LIGHT TANKS)
    "xiphos", "percutio", "gemini", "javelin", "hoplite", "peltast", "light",
    // ARTILLERÍA
    "actaeon", "vesta", "ixion",
    // ARMORED CAR (VEHÍCULOS BLINDADOS LIVIANOS)
    "drummond", "odyssey", "icarus",
    // CAMIONES DE TRANSPORTE (TRUCKS)
    "dunne", "leatherback", "loadlugger", "landrunner", "hauler", "sisyphus",
    "speartip", "retiarius",
    // TANQUES DE COMBUSTIBLE (FUEL/TANKER)
    "fuelrunner", "stolon", "tanker",
    // AMBULANCIAS (MEDICAL)
    "responder", "salus", "salva", "ambulance",
    // VEHÍCULOS ESPECIALES (SPECIAL)
    "dousing engine", "caravaner", "chariot",
    // GRÚAS Y VEHÍCULOS DE CONSTRUCCIÓN
    "assembly rig", "auto-crane", "packmule", "scrap hauler", "overseer",
    // BARCOS (BOATS)
    "grouper", "aquatipper", "ironship", "white whale",
    // GUNSHIPS (BUQUES DE GUERRA)
    "ronan gunship", "meteora", "charon", "bellweather", "krokodil",
    // AERONAVES (AIRCRAFT)
    "rinnspeir", "ornitier", "strider", "fathomer", "sombre", "blackguard", "lucian",
    "fighter aircraft", "scout", "bomber", "dive bomber", "heavy bomber", "transport aircraft",
    // TRENES (LOCOMOTIVES & CARS)
    "locomotive", "car", "flatbed car", "container car", "infantry car", "combat car",
    "artillery car", "first aid car", "caboose",
    // VEHÍCULOS RELIQUIAS (RELIC VEHICLES)
    "relic truck", "relic armoured car", "relic scout", "relic assault tank", "relic light tank",
    // VEHÍCULOS ESPECIALES FINALES
    "centurion", "herne", "scourge hunter"
};

static const std::vector<std::string> VEHICLE_BLACKLIST = {
    "static", "footprint", "smalltrainmaterial", "smalltrainshipping", "cardtable", "salvagefield", "Coverwall", "tankimpact", "explosion", "explosivelight", "directionallight", "tankstop"
};

// ─────────────────────────────────────────────────────────
// Check if a string contains any blacklist keyword
// ─────────────────────────────────────────────────────────
static bool IsBlacklisted(const std::string& classNameLower)
{
    for (const auto& blacklistedKeyword : VEHICLE_BLACKLIST) {
        if (classNameLower.find(blacklistedKeyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────
// Check if a string contains any vehicle keyword
// ─────────────────────────────────────────────────────────
static bool IsVehicleKeyword(const std::string& classNameLower)
{
    for (const auto& keyword : VEHICLE_KEYWORDS) {
        if (classNameLower.find(keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════
// ESP Update - Players
// ═══════════════════════════════════════════════════════════════
static void UpdateESP()
{
    if (!G::base) return;
    G::localPCAddr.store(0, std::memory_order_relaxed);

    SDK::UWorld* world = SDK::UWorld::GetWorld();
    if (!world) return;

    SDK::UGameInstance* gi = world->OwningGameInstance;
    if (!gi || gi->LocalPlayers.Num() == 0) return;

    SDK::ULocalPlayer* lp = gi->LocalPlayers[0];
    if (!lp) return;

    SDK::APlayerController* pc = reinterpret_cast<SDK::UPlayer*>(lp)->PlayerController;
    if (!pc) return;

    G::localPawn = reinterpret_cast<uintptr_t>(pc->AcknowledgedPawn);
    G::localPCAddr.store(reinterpret_cast<uintptr_t>(pc), std::memory_order_release);

    // Cache character class
    static uintptr_t s_lastWorld = 0;
    uintptr_t curWorld = reinterpret_cast<uintptr_t>(world);
    if (curWorld != s_lastWorld) {
        s_lastWorld = curWorld;
        G::charClass = 0;
        G::vehicleClassesDirty = true;
        G::structureClassesDirty = true;
    }

    if (!G::charClass && G::localPawn) {
        SDK::UObject* pawnObj = reinterpret_cast<SDK::UObject*>(G::localPawn);
        if (pawnObj && pawnObj->Class) {
            G::charClass = reinterpret_cast<uintptr_t>(pawnObj->Class);
        }
    }

    // Get local team e INVERTIR
    if (G::localPawn) {
        uint8_t rawTeam = reinterpret_cast<SDK::ASimCharacter*>(G::localPawn)->TeamId;
        G::localTeam = (int)InvertTeam(rawTeam);  // Invertir aquí
    }

    if (!G::charClass) return;

    int totalLevels = world->Levels.Num();
    if (totalLevels <= 0 || totalLevels > 512) return;

    std::vector<PlayerESP> found;
    std::vector<PlayerESP> foundForLines;
    found.reserve(64);
    foundForLines.reserve(64);

    FVec3 localPawnPos{};
    if (G::localPawn) GetWorldPos(G::localPawn, localPawnPos);

    for (int li = 0; li < totalLevels; ++li) {
        SDK::ULevel* level = world->Levels[li];
        if (!level || !IsValidPtr(reinterpret_cast<uintptr_t>(level))) continue;
        int actNum = level->Actors.Num();
        if (actNum <= 0 || actNum > 20000) continue;

        for (int i = 0; i < actNum; ++i) {
            SDK::AActor* actor = level->Actors[i];
            if (!actor || !IsValidPtr(reinterpret_cast<uintptr_t>(actor))) continue;

            if (reinterpret_cast<uintptr_t>(actor->Class) != G::charClass) continue;

            auto* pChar = reinterpret_cast<SDK::ASimCharacter*>(actor);

            float hp = pChar->Health;
            if (hp <= 0.f) continue;

            PlayerESP esp;
            if (!GetWorldPos(reinterpret_cast<uintptr_t>(actor), esp.pos)) continue;

            esp.hp = hp;
            esp.maxHP = pChar->MaxHealth;
            uint8_t rawTeam = pChar->TeamId;
            esp.team = InvertTeam(rawTeam);  // Invertir aquí también
            esp.actorAddr = reinterpret_cast<uintptr_t>(actor);
            esp.isLocal = (esp.actorAddr == G::localPawn);
            esp.dist = Dist3D(esp.pos, localPawnPos);

            // Read actor yaw
            {
                uintptr_t rootPtr = *reinterpret_cast<uintptr_t*>(
                    reinterpret_cast<uintptr_t>(pChar) + 0x130);
                if (IsValidPtr(rootPtr)) {
                    const float* q = reinterpret_cast<const float*>(rootPtr + 0xF0);
                    esp.viewYaw = atan2f(2.f * (q[3] * q[2] + q[0] * q[1]),
                        1.f - 2.f * (q[1] * q[1] + q[2] * q[2]));
                }
            }

            // Get skeleton bones
            if (G::showBones) {
                GetSkeletonBones(reinterpret_cast<uintptr_t>(actor), esp.bones);
            }

            if (esp.dist > G::maxDist) continue;
            if (esp.maxHP <= 0) esp.maxHP = 100;

            // SIEMPRE añadir el jugador local
            if (esp.isLocal) {
                found.push_back(esp);
                foundForLines.push_back(esp);
                continue;
            }

            // Verificar si es enemigo
            bool isEnemy = (G::localTeam >= 0) && ((int)esp.team != G::localTeam);

            // Con enemyOnly: SOLO añadir enemigos (no aliados, no líneas a aliados)
            if (G::enemyOnly) {
                if (isEnemy) {
                    found.push_back(esp);
                    foundForLines.push_back(esp);
                }
            }
            else {
                // Sin enemyOnly: añadir todos + líneas a todos (excepto a ti mismo)
                found.push_back(esp);
                foundForLines.push_back(esp);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(G::mtx);
        for (auto& ne : found) {
            for (const auto& old : G::players) {
                if (old.actorAddr == ne.actorAddr) {
                    ne.prevPos = old.pos;
                    ne.hasPrev = true;
                    break;
                }
            }
            if (!ne.hasPrev) ne.prevPos = ne.pos;
        }
        for (auto& ne : foundForLines) {
            for (const auto& old : G::playersNoFilter) {
                if (old.actorAddr == ne.actorAddr) {
                    ne.prevPos = old.pos;
                    ne.hasPrev = true;
                    break;
                }
            }
            if (!ne.hasPrev) ne.prevPos = ne.pos;
        }
        G::players = std::move(found);
        G::playersNoFilter = std::move(foundForLines);
    }
    G::lastScanTick.store(static_cast<uint32_t>(GetTickCount()), std::memory_order_release);
}

static void SafeUpdateESP()
{
    __try { UpdateESP(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ═══════════════════════════════════════════════════════════════
// Vehicle ESP - DETECCIÓN MEJORADA Y BALANCEADA
// ═══════════════════════════════════════════════════════════════
static int CollectUniqueClasses(uintptr_t* buf, int maxBuf)
{
    int count = 0;
    __try {
        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world) return 0;
        int totalLevels = world->Levels.Num();
        if (totalLevels <= 0 || totalLevels > 512) return 0;
        for (int li = 0; li < totalLevels; ++li) {
            SDK::ULevel* level = world->Levels[li];
            if (!level || !IsValidPtr(reinterpret_cast<uintptr_t>(level))) continue;
            int actNum = level->Actors.Num();
            if (actNum <= 0 || actNum > 30000) continue;
            for (int i = 0; i < actNum && count < maxBuf; ++i) {
                SDK::AActor* actor = level->Actors[i];
                if (!actor || !IsValidPtr(reinterpret_cast<uintptr_t>(actor))) continue;
                uintptr_t cls = reinterpret_cast<uintptr_t>(actor->Class);
                if (!IsValidPtr(cls)) continue;
                bool dup = false;
                for (int j = 0; j < count; ++j) if (buf[j] == cls) { dup = true; break; }
                if (!dup) buf[count++] = cls;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

static void CacheVehicleClasses()
{
    static uintptr_t clsBuf[2048];
    int clsCount = CollectUniqueClasses(clsBuf, 2048);

    std::vector<uintptr_t> matched;

    printf("[ESP] Scanning %d unique classes for vehicles...\n", clsCount);

    for (int i = 0; i < clsCount; ++i) {
        try {
            auto* obj = reinterpret_cast<SDK::UObject*>(clsBuf[i]);
            std::string className = obj->GetName();
            std::string classNameLower = ToLower(className);

            bool isVehicle = false;
            std::string reason = "";

            // ─────────────────────────────────────────────────────────
            // FILTRO 1: BLACKLIST (PRIMERO - RECHAZA RÁPIDO)
            // ─────────────────────────────────────────────────────────
            if (IsBlacklisted(classNameLower)) {
                continue;
            }

            // ─────────────────────────────────────────────────────────
            // FILTRO 2: HERENCIA (MÁS CONFIABLE)
            // Busca si hereda de SimVehicle o clase vehicular base
            // ─────────────────────────────────────────────────────────
            {
                uintptr_t cur = clsBuf[i];
                for (int depth = 0; depth < 32 && IsValidPtr(cur); ++depth) {
                    try {
                        auto* uobj = reinterpret_cast<SDK::UObject*>(cur);
                        std::string nm = uobj->GetName();
                        std::string nmLower = ToLower(nm);

                        // Clases base conocidas de vehículos
                        if (nm == "SimVehicle" ||
                            nm == "Vehicle_C" ||
                            nmLower == "base_vehicle" ||
                            nmLower.find("vehiclebase") != std::string::npos) {
                            isVehicle = true;
                            reason = "inheritance: " + nm;
                            break;
                        }

                        uintptr_t super = *reinterpret_cast<uintptr_t*>(cur + 0x0040);
                        if (!IsValidPtr(super) || super == cur) break;
                        cur = super;
                    }
                    catch (...) {
                        break;
                    }
                }
            }

            // Si ya detectamos por herencia, no continuar
            if (isVehicle) {
                matched.push_back(clsBuf[i]);
                printf("[ESP] Vehicle class found: %s (%s)\n", className.c_str(), reason.c_str());
                continue;
            }

            // ─────────────────────────────────────────────────────────
            // FILTRO 3: PALABRAS CLAVE VEHICULARES (LISTA BASADA)
            // ─────────────────────────────────────────────────────────
            if (IsVehicleKeyword(classNameLower)) {
                isVehicle = true;
                reason = "keyword match";
                matched.push_back(clsBuf[i]);
                printf("[ESP] Vehicle class found: %s (%s)\n", className.c_str(), reason.c_str());
            }
        }
        catch (...) {}
    }

    {
        std::lock_guard<std::mutex> lk(G::mtx);
        G::vehicleClasses = std::move(matched);
    }

    printf("[ESP] Total vehicle classes cached: %zu\n", G::vehicleClasses.size());
}

struct VehicleRaw {
    FVec3     pos;
    float     dist;
    int       health;
    uint8_t   teamId;
    uintptr_t clsPtr;
};
static const int VEH_MAX = 512;

static int CollectVehicleRaw(VehicleRaw* buf, int maxBuf,
    const uintptr_t* classes, int classCount,
    float maxDistCm)
{
    int count = 0;
    __try {
        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world) return 0;
        int totalLevels = world->Levels.Num();
        if (totalLevels <= 0 || totalLevels > 512) return 0;

        FVec3 lpPos{};
        if (G::localPawn)
            GetWorldPos(G::localPawn, lpPos);

        for (int li = 0; li < totalLevels; ++li) {
            SDK::ULevel* level = world->Levels[li];
            if (!level || !IsValidPtr(reinterpret_cast<uintptr_t>(level))) continue;
            int actNum = level->Actors.Num();
            if (actNum <= 0 || actNum > 30000) continue;

            for (int i = 0; i < actNum && count < maxBuf; ++i) {
                SDK::AActor* actor = level->Actors[i];
                if (!actor || !IsValidPtr(reinterpret_cast<uintptr_t>(actor))) continue;
                uintptr_t cls = reinterpret_cast<uintptr_t>(actor->Class);

                bool isVeh = false;
                for (int ci = 0; ci < classCount; ++ci)
                    if (classes[ci] == cls) { isVeh = true; break; }
                if (!isVeh) continue;

                FVec3 pos{};
                if (!GetWorldPos(reinterpret_cast<uintptr_t>(actor), pos)) continue;

                float dist = 0.f;
                if (G::localPawn)
                    dist = Dist3D(pos, lpPos);

                if (maxDistCm > 0.f && dist > maxDistCm) continue;

                uintptr_t abase = reinterpret_cast<uintptr_t>(actor);

                // Intentar múltiples offsets para health
                int health = 0;
                __try {
                    // Offset 1: 0x0B08
                    int h1 = *reinterpret_cast<int32_t*>(abase + 0x0B08);
                    if (h1 > 0 && h1 < 100000) {
                        health = h1;
                    }
                    else {
                        // Offset 2: 0x3BC (estructura)
                        int h2 = *reinterpret_cast<int32_t*>(abase + 0x3BC);
                        if (h2 > 0 && h2 < 100000) {
                            health = h2;
                        }
                        else {
                            // Offset 3: Buscar en memoria cercana
                            health = 50; // Default si no encuentra
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    health = 50;
                }

                uint8_t tid = 255;
                __try {
                    tid = *reinterpret_cast<uint8_t*>(abase + 0x0B2F);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                    tid = 255;
                }

                if (health > 0)
                    buf[count++] = { pos, dist, health, tid, cls };
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

static void UpdateVehicleESP()
{
    bool needCache = false;
    {
        std::lock_guard<std::mutex> lk(G::mtx);
        needCache = G::vehicleClassesDirty;
        if (needCache) G::vehicleClassesDirty = false;
    }
    if (needCache) CacheVehicleClasses();

    std::vector<uintptr_t> classes;
    {
        std::lock_guard<std::mutex> lk(G::mtx);
        classes = G::vehicleClasses;
    }
    if (classes.empty()) return;

    static VehicleRaw raw[VEH_MAX];
    float maxDistCm = G::vehicleMaxDist * 100.f;
    int count = CollectVehicleRaw(raw, VEH_MAX, classes.data(), (int)classes.size(), maxDistCm);

    std::vector<VehicleESP> found;
    found.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (raw[i].health <= 0) continue;

        uint8_t invertedTeam = InvertTeam(raw[i].teamId);
        if (G::vehicleEnemyOnly && G::localTeam >= 0)
            if ((int)invertedTeam == G::localTeam) continue;

        VehicleESP v;
        v.pos = raw[i].pos;
        v.dist = raw[i].dist;
        v.health = raw[i].health;
        v.teamId = invertedTeam;
        try {
            auto* cls = reinterpret_cast<SDK::UObject*>(raw[i].clsPtr);
            std::string nm = cls->GetName();
            snprintf(v.clsName, sizeof(v.clsName), "%s", nm.c_str());
        }
        catch (...) {
            snprintf(v.clsName, sizeof(v.clsName), "<vehicle>");
        }
        found.push_back(v);
    }

    std::lock_guard<std::mutex> lk(G::mtx);
    G::vehicles = std::move(found);
}

static void SafeUpdateVehicleESP()
{
    try { UpdateVehicleESP(); }
    catch (...) {}
}

// ═══════════════════════════════════════════════════════════════
// Auto Vehicle Class Caching (every 5 seconds)
// ═══════════════════════════════════════════════════════════════
static void AutoCacheVehicleClasses()
{
    uint32_t now = static_cast<uint32_t>(GetTickCount());
    uint32_t last = G::lastVehicleClassScan.load(std::memory_order_acquire);

    // Scan every 5 seconds (5000ms)
    if ((now - last) >= 5000 || last == 0) {
        printf("[ESP] Auto-caching vehicle classes (5s interval)...\n");
        CacheVehicleClasses();
        G::lastVehicleClassScan.store(now, std::memory_order_release);
    }
}

// ═══════════════════════════════════════════════════════════════
// Vehicle Proximity Detection (every 100ms)
// ═══════════════════════════════════════════════════════════════
static void UpdateVehicleProximity()
{
    bool needCache = false;
    {
        std::lock_guard<std::mutex> lk(G::mtx);
        needCache = G::vehicleClassesDirty;
        if (needCache) G::vehicleClassesDirty = false;
    }
    if (needCache) CacheVehicleClasses();

    std::vector<uintptr_t> classes;
    {
        std::lock_guard<std::mutex> lk(G::mtx);
        classes = G::vehicleClasses;
    }
    if (classes.empty()) return;

    static VehicleRaw raw[VEH_MAX];
    float maxDistCm = G::vehicleProximityDist * 100.f;
    int count = CollectVehicleRaw(raw, VEH_MAX, classes.data(), (int)classes.size(), maxDistCm);

    std::vector<NearbyVehicle> found;
    found.reserve(count);

    for (int i = 0; i < count; ++i) {
        if (raw[i].health <= 0) continue;

        uint8_t invertedTeam = InvertTeam(raw[i].teamId);

        NearbyVehicle nv;
        nv.pos = raw[i].pos;
        nv.dist = raw[i].dist;
        nv.health = raw[i].health;
        nv.teamId = invertedTeam;
        try {
            auto* cls = reinterpret_cast<SDK::UObject*>(raw[i].clsPtr);
            std::string nm = cls->GetName();
            snprintf(nv.clsName, sizeof(nv.clsName), "%s", nm.c_str());
        }
        catch (...) {
            snprintf(nv.clsName, sizeof(nv.clsName), "<vehicle>");
        }
        found.push_back(nv);
    }

    {
        std::lock_guard<std::mutex> lk(G::mtx);
        G::nearbyVehicles = std::move(found);
    }
    G::lastVehicleProximityScan.store(static_cast<uint32_t>(GetTickCount()), std::memory_order_release);
}

static void SafeUpdateVehicleProximity()
{
    try { UpdateVehicleProximity(); }
    catch (...) {}
}

// ═══════════════════════════════════════════════════════════════
// Structure / Building ESP
// ═══════════════════════════════════════════════════════════════
static void CacheStructureClasses()
{
    static uintptr_t clsBuf[2048];
    int clsCount = CollectUniqueClasses(clsBuf, 2048);

    std::vector<uintptr_t> struc;
    for (int i = 0; i < clsCount; ++i) {
        try {
            bool isStructure = false;
            uintptr_t cur = clsBuf[i];
            for (int depth = 0; depth < 24 && IsValidPtr(cur); ++depth) {
                auto* uobj = reinterpret_cast<SDK::UObject*>(cur);
                std::string nm = uobj->GetName();
                if (nm == "Structure") isStructure = true;
                uintptr_t super = *reinterpret_cast<uintptr_t*>(cur + 0x0040);
                if (!IsValidPtr(super) || super == cur) break;
                cur = super;
            }
            if (isStructure) struc.push_back(clsBuf[i]);
        }
        catch (...) {}
    }

    {
        std::lock_guard<std::mutex> lk(G::mtx);
        G::structureClasses = std::move(struc);
    }
}

struct StructureRaw {
    FVec3     pos;
    float     dist;
    int       health;
    int       maxHealth;
    uint8_t   teamId;
    uintptr_t clsPtr;
};
static const int STR_MAX = 1024;

static int CollectStructureRaw(StructureRaw* buf, int maxBuf,
    const uintptr_t* classes, int classCount,
    float maxDistCm)
{
    int count = 0;
    __try {
        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world) return 0;
        int totalLevels = world->Levels.Num();
        if (totalLevels <= 0 || totalLevels > 512) return 0;

        FVec3 lpPos{};
        if (G::localPawn)
            GetWorldPos(G::localPawn, lpPos);

        for (int li = 0; li < totalLevels; ++li) {
            SDK::ULevel* level = world->Levels[li];
            if (!level || !IsValidPtr(reinterpret_cast<uintptr_t>(level))) continue;
            int actNum = level->Actors.Num();
            if (actNum <= 0 || actNum > 30000) continue;

            for (int i = 0; i < actNum && count < maxBuf; ++i) {
                SDK::AActor* actor = level->Actors[i];
                if (!actor || !IsValidPtr(reinterpret_cast<uintptr_t>(actor))) continue;
                uintptr_t cls = reinterpret_cast<uintptr_t>(actor->Class);

                bool isStr = false;
                for (int ci = 0; ci < classCount; ++ci)
                    if (classes[ci] == cls) { isStr = true; break; }
                if (!isStr) continue;

                FVec3 pos{};
                if (!GetWorldPos(reinterpret_cast<uintptr_t>(actor), pos)) continue;

                float dist = 0.f;
                if (G::localPawn)
                    dist = Dist3D(pos, lpPos);

                if (maxDistCm > 0.f && dist > maxDistCm) continue;

                uintptr_t abase = reinterpret_cast<uintptr_t>(actor);
                int     health = *reinterpret_cast<int32_t*>(abase + 0x3BC);
                int     maxHealth = *reinterpret_cast<int32_t*>(abase + 0x3B8);
                uint8_t tid = *reinterpret_cast<uint8_t*>(abase + 0x860);

                if (health > 0)
                    buf[count++] = { pos, dist, health, maxHealth, tid, cls };
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

static void UpdateStructureESP()
{
    bool needCache = false;
    {
        std::lock_guard<std::mutex> lk(G::mtx);
        needCache = G::structureClassesDirty;
        if (needCache) G::structureClassesDirty = false;
    }
    if (needCache) CacheStructureClasses();

    std::vector<uintptr_t> classes;
    {
        std::lock_guard<std::mutex> lk(G::mtx);
        classes = G::structureClasses;
    }
    if (classes.empty()) return;

    static StructureRaw raw[STR_MAX];
    float maxDistCm = G::structureMaxDist * 100.f;
    int count = CollectStructureRaw(raw, STR_MAX,
        classes.data(), (int)classes.size(),
        maxDistCm);

    std::vector<StructureESP> found;
    found.reserve(count);
    for (int i = 0; i < count; ++i) {
        if (raw[i].health <= 0) continue;

        uint8_t invertedTeam = InvertTeam(raw[i].teamId);
        if (G::structureEnemyOnly && G::localTeam >= 0)
            if ((int)invertedTeam == G::localTeam) continue;

        StructureESP s;
        s.pos = raw[i].pos;
        s.dist = raw[i].dist;
        s.health = raw[i].health;
        s.maxHealth = raw[i].maxHealth;
        s.teamId = invertedTeam;
        try {
            auto* cls = reinterpret_cast<SDK::UObject*>(raw[i].clsPtr);
            std::string nm = cls->GetName();
            snprintf(s.clsName, sizeof(s.clsName), "%s", nm.c_str());
        }
        catch (...) {
            snprintf(s.clsName, sizeof(s.clsName), "<structure>");
        }
        found.push_back(s);
    }

    std::lock_guard<std::mutex> lk(G::mtx);
    G::structures = std::move(found);
}
static HWND                   g_hWnd = nullptr;
static void SafeUpdateStructureESP()
{
    try { UpdateStructureESP(); }
    catch (...) {}
}

// ═══════════════════════════════════════════════════════════════
// Autoaim - Find best target
// ═══════════════════════════════════════════════════════════════
static uintptr_t FindBestAutoaimTarget(float maxScreenDist = 150.f)
{
    if (!G::enableAutoaim) return 0;

    RECT cr{};
    GetClientRect(g_hWnd, &cr);
    float sw = (float)cr.right, sh = (float)cr.bottom;

    // Posición del cursor
    POINT cursorPos;
    GetCursorPos(&cursorPos);
    ScreenToClient(g_hWnd, &cursorPos);
    float curX = (float)cursorPos.x;
    float curY = (float)cursorPos.y;

    uintptr_t bestTarget = 0;
    float bestDist = maxScreenDist;

    std::vector<PlayerESP> players;
    {
        std::lock_guard<std::mutex> lk(G::mtx);
        players = G::players;
    }

    for (const auto& p : players) {
        if (p.isLocal) continue;
        if (G::autoaimEnemyOnly) {
            bool isEnemy = (G::localTeam >= 0) && ((int)p.team != G::localTeam);
            if (!isEnemy) continue;
        }

        if (p.bones.size() < 2) continue;

        // Seleccionar bone (0 = head, 1 = torso/central)
        size_t boneIdx = (G::autoaimBoneTarget == 0) ? 0 : 1;
        if (boneIdx >= p.bones.size() || !p.bones[boneIdx].valid) continue;

        float boneScreenX, boneScreenY;
        if (!SafeW2S(p.bones[boneIdx].pos, boneScreenX, boneScreenY)) continue;

        // Distancia en pantalla desde el cursor
        float dx = boneScreenX - curX;
        float dy = boneScreenY - curY;
        float screenDist = sqrtf(dx * dx + dy * dy);

        if (screenDist < bestDist) {
            bestDist = screenDist;
            bestTarget = p.actorAddr;
        }
    }

    return bestTarget;
}

// ─────────────────────────────────────────────────────────
// Get screen position of autoaim target bone
// ─────────────────────────────────────────────────────────
static bool GetAutoaimTargetScreenPos(uintptr_t targetAddr, FVec3& outScreenPos)
{
    if (!IsValidPtr(targetAddr)) return false;

    std::vector<PlayerESP> players;
    {
        std::lock_guard<std::mutex> lk(G::mtx);
        players = G::players;
    }

    for (const auto& p : players) {
        if (p.actorAddr != targetAddr) continue;
        if (p.bones.size() < 2) return false;

        size_t boneIdx = (G::autoaimBoneTarget == 0) ? 0 : 1;
        if (boneIdx >= p.bones.size() || !p.bones[boneIdx].valid) return false;

        if (SafeW2S(p.bones[boneIdx].pos, outScreenPos.x, outScreenPos.y)) {
            outScreenPos.z = 0.f;
            return true;
        }
        return false;
    }

    return false;
}

// ─────────────────────────────────────────────────────────
// Apply autoaim smoothing and move mouse
// ─────────────────────────────────────────────────────────
static void ApplyAutoaimSmoothing()
{
    if (!G::autoaimActive || !IsValidPtr(G::autoaimTargetAddr)) {
        G::autoaimActive = false;
        G::autoaimTargetAddr = 0;
        return;
    }

    FVec3 targetScreenPos{};
    if (!GetAutoaimTargetScreenPos(G::autoaimTargetAddr, targetScreenPos)) {
        G::autoaimActive = false;
        return;
    }

    POINT cursorPos;
    GetCursorPos(&cursorPos);

    float curX = (float)cursorPos.x;
    float curY = (float)cursorPos.y;
    float targetX = targetScreenPos.x;
    float targetY = targetScreenPos.y;

    // Aplicar smoothing (interpolación lineal)
    float smoothFactor = G::autoaimSmoothing;
    float newX = curX + (targetX - curX) * smoothFactor;
    float newY = curY + (targetY - curY) * smoothFactor;

    // Mover el mouse
    SetCursorPos((int)newX, (int)newY);
}

// ═══════════════════════════════════════════════════════════════
// D3D11 Hook
// ═══════════════════════════════════════════════════════════════
using FnPresent = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using FnResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static FnPresent              g_oPresent = nullptr;
static FnResizeBuffers        g_oResizeBuffers = nullptr;
static void** g_scVTable = nullptr;
static ID3D11Device* g_pDevice = nullptr;
static ID3D11DeviceContext* g_pCtx = nullptr;
static ID3D11RenderTargetView* g_pRTV = nullptr;
static volatile bool          g_initDone = false;
static DWORD WINAPI WorkerThread(LPVOID);
static volatile bool          g_resizing = false;
static volatile bool          g_doUnload = false;
static volatile bool          g_unloadRequested = false;
static HMODULE                g_hSelf = nullptr;
static WNDPROC                g_oWndProc = nullptr;

static void CreateRTV(IDXGISwapChain* sc)
{
    if (g_pRTV) { g_pRTV->Release(); g_pRTV = nullptr; }
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        g_pDevice->CreateRenderTargetView(bb, nullptr, &g_pRTV);
        bb->Release();
    }
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(
    IDXGISwapChain* sc, UINT bufCnt, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    g_resizing = true;
    if (g_pCtx)  g_pCtx->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_pRTV) { g_pRTV->Release(); g_pRTV = nullptr; }
    HRESULT hr = g_oResizeBuffers(sc, bufCnt, w, h, fmt, flags);
    if (SUCCEEDED(hr) && g_pDevice) CreateRTV(sc);
    g_resizing = false;
    return hr;
}

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (G::showMenu && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return TRUE;
    return CallWindowProcA(g_oWndProc, hWnd, msg, wParam, lParam);
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (g_doUnload || g_resizing) return g_oPresent(sc, sync, flags);

    if (g_unloadRequested && g_initDone) {
        g_unloadRequested = false;

        if (g_oWndProc) {
            SetWindowLongPtrA(g_hWnd, GWLP_WNDPROC, (LONG_PTR)g_oWndProc);
            g_oWndProc = nullptr;
        }

        if (g_pCtx) g_pCtx->OMSetRenderTargets(0, nullptr, nullptr);
        if (g_pRTV) { g_pRTV->Release(); g_pRTV = nullptr; }
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_pCtx) { g_pCtx->Release();    g_pCtx = nullptr; }
        if (g_pDevice) { g_pDevice->Release(); g_pDevice = nullptr; }

        g_initDone = false;
        g_doUnload = true;
        printf("[ESP] Teardown complete\n");
        return g_oPresent(sc, sync, flags);
    }

    if (!g_initDone) {
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_pDevice))) {
            g_pDevice->GetImmediateContext(&g_pCtx);

            DXGI_SWAP_CHAIN_DESC sd{};
            sc->GetDesc(&sd);
            g_hWnd = sd.OutputWindow;

            g_oWndProc = (WNDPROC)SetWindowLongPtrA(
                g_hWnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

            CreateRTV(sc);

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

            ImGui::StyleColorsDark();
            ImGuiStyle& s = ImGui::GetStyle();
            s.WindowRounding = 7.f;
            s.FrameRounding = 4.f;

            ImGui_ImplWin32_Init(g_hWnd);
            ImGui_ImplDX11_Init(g_pDevice, g_pCtx);
            g_initDone = true;
            printf("[ESP] ImGui initialized\n");

            CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        }
    }
    if (!g_initDone) return g_oPresent(sc, sync, flags);

    // ── INSERT key toggle ────────────────────────────────────
    static bool insLast = false;
    bool insNow = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insNow && !insLast) {
        G::showMenu = !G::showMenu;
        ImGui::GetIO().MouseDrawCursor = G::showMenu;
        if (G::showMenu)  while (ShowCursor(TRUE) < 0) {}
        else              while (ShowCursor(FALSE) >= 0) {}
    }
    insLast = insNow;

    // ── AUTOAIM activation key toggle ────────────────────────────────
    static bool autoaimKeyLast = false;
    bool autoaimKeyNow = (GetAsyncKeyState(G::autoaimActivationKey) & 0x8000) != 0;

    if (autoaimKeyNow && !autoaimKeyLast && G::enableAutoaim) {
        // Buscar objetivo más cercano al cursor
        uintptr_t newTarget = FindBestAutoaimTarget(150.f);
        if (newTarget != 0) {
            G::autoaimTargetAddr = newTarget;
            G::autoaimActive = true;
        }
    }
    else if (!autoaimKeyNow && autoaimKeyLast) {
        // Soltar la tecla desactiva el autoaim
        G::autoaimActive = false;
    }
    autoaimKeyLast = autoaimKeyNow;

    // ── Apply autoaim smoothing if active ────────────────────────────
    if (G::autoaimActive && G::enableAutoaim) {
        ApplyAutoaimSmoothing();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // ─────────────────────────────────────────────────────────
    // ESP Render
    // ─────────────────────────────────────────────────────────
    if (G::espEnabled) {
        RECT cr{};
        GetClientRect(g_hWnd, &cr);
        int sw = cr.right, sh = cr.bottom;

        if (sw > 0 && sh > 0) {
            ImDrawList* dl = ImGui::GetBackgroundDrawList();

            std::vector<PlayerESP> players;
            std::vector<PlayerESP> playersForLines;
            FVec3 localPawnPosScreen{};
            float localScreenX = (float)sw * 0.5f;  // Centro de pantalla por defecto
            float localScreenY = (float)sh * 0.5f;
            {
                std::lock_guard<std::mutex> lk(G::mtx);
                players = G::players;
                playersForLines = G::playersNoFilter;

                // Obtener posición del jugador local en pantalla
                for (const auto& p : playersForLines) {
                    if (p.isLocal) {
                        localPawnPosScreen = p.pos;
                        if (!SafeW2S(p.pos, localScreenX, localScreenY)) {
                            // Si W2S falla, usar centro de pantalla
                            localScreenX = (float)sw * 0.5f;
                            localScreenY = (float)sh * 0.5f;
                        }
                        break;
                    }
                }
            }

            float espT;
            {
                uint32_t now = static_cast<uint32_t>(GetTickCount());
                uint32_t last = G::lastScanTick.load(std::memory_order_acquire);
                espT = static_cast<float>(now - last) * (1.f / 50.f);
                if (espT < 0.f) espT = 0.f;
                if (espT > 1.2f) espT = 1.2f;
            }

            // ── Dibujar líneas PRIMERO (antes de todo) ──
            if (G::showLines) {
                for (const auto& p : playersForLines) {
                    if (p.isLocal) continue;

                    FVec3 ipos;
                    if (p.hasPrev) {
                        ipos = { p.prevPos.x + (p.pos.x - p.prevPos.x) * espT,
                                 p.prevPos.y + (p.pos.y - p.prevPos.y) * espT,
                                 p.prevPos.z + (p.pos.z - p.prevPos.z) * espT };
                    }
                    else {
                        ipos = p.pos;
                    }

                    float sx, sy;
                    if (!SafeW2S(ipos, sx, sy)) continue;

                    // Determinar si es enemigo (mismo que en la visualización)
                    bool isEnemy = (G::localTeam >= 0) && ((int)p.team != G::localTeam);

                    // Elegir color según el team
                    ImU32 col;
                    if (p.team == 0)  // Warden - AZUL
                        col = isEnemy ? IM_COL32(50, 150, 255, 230) : IM_COL32(70, 130, 200, 180);
                    else if (p.team == 1)  // Colonial - VERDE
                        col = isEnemy ? IM_COL32(50, 255, 100, 230) : IM_COL32(80, 200, 100, 180);
                    else
                        col = IM_COL32(200, 200, 200, 180);

                    dl->AddLine({ localScreenX, localScreenY }, { sx, sy }, col, 1.5f);
                }
            }

            for (const auto& p : players) {
                // ── Local player health (top center) ──
                if (p.isLocal) {
                    float frac = (p.maxHP > 0) ? (p.hp / (float)p.maxHP) : 1.f;
                    frac = (frac < 0.f) ? 0.f : (frac > 1.f ? 1.f : frac);

                    float bw = 200.f, bh = 20.f;
                    float bx = (float)sw * 0.5f - bw * 0.5f;
                    float by = 20.f;

                    dl->AddRectFilled({ bx, by }, { bx + bw, by + bh }, IM_COL32(25, 25, 25, 200), 3.f);

                    ImU32 hcol = (frac > 0.6f) ? IM_COL32(70, 210, 70, 255)
                        : (frac > 0.3f) ? IM_COL32(220, 190, 35, 255)
                        : IM_COL32(215, 50, 50, 255);
                    if (frac > 0.f)
                        dl->AddRectFilled({ bx, by }, { bx + bw * frac, by + bh }, hcol, 3.f);

                    dl->AddRect({ bx, by }, { bx + bw, by + bh }, IM_COL32(0, 0, 0, 150), 3.f);

                    char hpText[32];
                    snprintf(hpText, sizeof(hpText), "%.0f/%.0f", p.hp, (float)p.maxHP);

                    float fontSize = ImGui::GetFontSize() * 1.8f;
                    ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(
                        fontSize, FLT_MAX, 0.f, hpText);

                    float hpTextX = bx + bw * 0.5f - textSize.x * 0.5f;
                    float hpTextY = by + bh * 0.5f - textSize.y * 0.5f;

                    dl->AddText(ImGui::GetFont(), fontSize,
                        { hpTextX + 2.f, hpTextY + 2.f }, IM_COL32(0, 0, 0, 220), hpText);
                    dl->AddText(ImGui::GetFont(), fontSize,
                        { hpTextX, hpTextY }, IM_COL32(255, 255, 255, 240), hpText);

                    continue;
                }

                FVec3 ipos;
                if (p.hasPrev) {
                    ipos = { p.prevPos.x + (p.pos.x - p.prevPos.x) * espT,
                             p.prevPos.y + (p.pos.y - p.prevPos.y) * espT,
                             p.prevPos.z + (p.pos.z - p.prevPos.z) * espT };
                }
                else {
                    ipos = p.pos;
                }

                float sx, sy;
                if (!SafeW2S(ipos, sx, sy)) continue;

                // Team color - CAMBIO AQUI
                bool isEnemy = (G::localTeam >= 0) && ((int)p.team != G::localTeam);
                ImU32 col;
                if (p.team == 0)  // Warden - AZUL
                    col = isEnemy ? IM_COL32(50, 150, 255, 230) : IM_COL32(70, 130, 200, 180);
                else if (p.team == 1)  // Colonial - VERDE
                    col = isEnemy ? IM_COL32(50, 255, 100, 230) : IM_COL32(80, 200, 100, 180);
                else
                    col = IM_COL32(200, 200, 200, 180);

                float distM = p.dist * 0.01f;

                // ── Skeleton bones - ESTRUCTURA ESQUELÉTICA SIMPLIFICADA ───────────────────────────
                if (G::showBones && p.bones.size() >= 7) {
                    // Dibujar puntos de los bones
                    for (size_t i = 0; i < p.bones.size(); ++i) {
                        if (!p.bones[i].valid) continue;

                        float bsx, bsy;
                        if (SafeW2S(p.bones[i].pos, bsx, bsy)) {
                            // Dibujar punto del bone
                            dl->AddCircleFilled({ bsx, bsy }, 2.5f, G::boneColor);
                        }
                    }

                    // Dibujar conexiones entre bones (estructura esquelética simplificada)
                    // Conexiones principales: cabeza-torso-entrepierna-piernas

                    // Cabeza -> Torso
                    DrawBoneConnection(dl, p.bones, 0, 1, G::boneColor);

                    // Torso -> Entrepierna
                    DrawBoneConnection(dl, p.bones, 1, 2, G::boneColor);

                    // Entrepierna -> Rodillas
                    DrawBoneConnection(dl, p.bones, 2, 3, G::boneColor);  // Entrepierna -> Rodilla izq
                    DrawBoneConnection(dl, p.bones, 2, 4, G::boneColor);  // Entrepierna -> Rodilla der

                    // Rodillas -> Pies
                    DrawBoneConnection(dl, p.bones, 3, 5, G::boneColor);  // Rodilla izq -> Pie izq
                    DrawBoneConnection(dl, p.bones, 4, 6, G::boneColor);  // Rodilla der -> Pie der
                }

                // ── Health bar ───────────────────────────────
                if (G::showHPBar) {
                    float frac = (p.maxHP > 0) ? (p.hp / (float)p.maxHP) : 1.f;
                    frac = (frac < 0.f) ? 0.f : (frac > 1.f ? 1.f : frac);

                    float bw = 120.f, bh = 16.f;
                    float bx = sx - bw * 0.5f, by = sy - 32.f;

                    dl->AddRectFilled({ bx, by }, { bx + bw, by + bh }, IM_COL32(25, 25, 25, 180), 3.f);

                    ImU32 hcol = (frac > 0.6f) ? IM_COL32(70, 210, 70, 255)
                        : (frac > 0.3f) ? IM_COL32(220, 190, 35, 255)
                        : IM_COL32(215, 50, 50, 255);
                    if (frac > 0.f)
                        dl->AddRectFilled({ bx, by }, { bx + bw * frac, by + bh }, hcol, 3.f);

                    dl->AddRect({ bx, by }, { bx + bw, by + bh }, IM_COL32(0, 0, 0, 120), 3.f);

                    char hpText[32];
                    snprintf(hpText, sizeof(hpText), "%.0f/%.0f", p.hp, (float)p.maxHP);

                    float fontSize = ImGui::GetFontSize() * 1.5f;
                    ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(
                        fontSize, FLT_MAX, 0.f, hpText);

                    float hpTextX = bx + bw * 0.5f - textSize.x * 0.5f;
                    float hpTextY = by + bh * 0.5f - textSize.y * 0.5f;

                    dl->AddText(ImGui::GetFont(), fontSize,
                        { hpTextX + 1.5f, hpTextY + 1.5f }, IM_COL32(0, 0, 0, 220), hpText);
                    dl->AddText(ImGui::GetFont(), fontSize,
                        { hpTextX, hpTextY }, IM_COL32(255, 255, 255, 240), hpText);
                }

                // ── Dot marker ───────────────────────────────
                dl->AddCircleFilled({ sx, sy }, 4.f, col);
                dl->AddCircle({ sx, sy }, 4.f, IM_COL32(0, 0, 0, 180), 0, 1.5f);

                // ── Distance label ───────────────────────────
                if (G::showDist) {
                    char tag[64];
                    const char* teamStr = (p.team == 0) ? "W" : (p.team == 1) ? "C" : "?";
                    snprintf(tag, sizeof(tag), "[%s] %.0fm", teamStr, distM);
                    dl->AddText({ sx + 7.f, sy - 7.f }, col, tag);
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────
    // Vehicle ESP render
    // ─────────────────────────────────────────────────────────
    if (G::showVehicleESP) {
        RECT vcr{};
        GetClientRect(g_hWnd, &vcr);
        int vsw = vcr.right, vsh = vcr.bottom;
        if (vsw > 0 && vsh > 0) {
            ImDrawList* vdl = ImGui::GetBackgroundDrawList();
            std::vector<VehicleESP> vehs;
            {
                std::lock_guard<std::mutex> lk(G::mtx);
                vehs = G::vehicles;
            }
            for (const auto& v : vehs) {
                float vsx, vsy;
                if (!SafeW2S(v.pos, vsx, vsy)) continue;
                if (vsx < -400.f || vsx >(float)vsw + 400.f ||
                    vsy < -400.f || vsy >(float)vsh + 400.f) continue;

                float distM = v.dist * 0.01f;

                bool isEnemy = (G::localTeam >= 0) && ((int)v.teamId != G::localTeam);
                ImU32 vcol;
                if (v.teamId == 0 && isEnemy)  vcol = IM_COL32(50, 150, 255, 230);  // Warden - AZUL
                else if (v.teamId == 0)              vcol = IM_COL32(70, 130, 200, 190);
                else if (v.teamId == 1 && isEnemy)  vcol = IM_COL32(50, 255, 100, 230);  // Colonial - VERDE
                else if (v.teamId == 1)              vcol = IM_COL32(80, 200, 100, 190);
                else                                 vcol = IM_COL32(200, 200, 200, 180);

                ImU32 shad = IM_COL32(0, 0, 0, 210);

                vdl->AddCircleFilled({ vsx, vsy }, 3.f, vcol);
                vdl->AddCircle({ vsx, vsy }, 3.f, shad, 8, 1.f);

                const char* teamTag = (v.teamId == 0) ? "W" : (v.teamId == 1) ? "C" : "?";
                char tag[80];
                snprintf(tag, sizeof(tag), "[%s] %s  [%.0fm]  H: %d",
                    teamTag, v.clsName, distM, v.health);

                float tx = vsx + 7.f;
                float ty = vsy - 9.f;

                // ─────────────────────────────────────────────────────────
                // NUEVO: Fuente 50% más grande
                // ─────────────────────────────────────────────────────────
                float fontSize = ImGui::GetFontSize() * 1.5f;

                vdl->AddText(ImGui::GetFont(), fontSize, { tx + 1.f, ty + 1.f }, shad, tag);
                vdl->AddText(ImGui::GetFont(), fontSize, { tx,       ty }, vcol, tag);

                // ─────────────────────────────────────────────────────────
                // NUEVO: Mostrar salud en negrita (separada y más grande)
                // ─────────────────────────────────────────────────────────
                char healthText[32];
                snprintf(healthText, sizeof(healthText), "H: %d", v.health);

                float healthFontSize = ImGui::GetFontSize() * 1.8f;
                ImVec2 healthSize = ImGui::GetFont()->CalcTextSizeA(
                    healthFontSize, FLT_MAX, 0.f, healthText);

                float healthX = vsx - healthSize.x * 0.5f;
                float healthY = vsy + 15.f;

                vdl->AddText(ImGui::GetFont(), healthFontSize,
                    { healthX + 1.5f, healthY + 1.5f }, shad, healthText);
                vdl->AddText(ImGui::GetFont(), healthFontSize,
                    { healthX, healthY }, IM_COL32(255, 100, 100, 255), healthText);
            }
        }
    }

    // ─────────────────────────────────────────────────────────
    // Structure ESP render
    // ─────────────────────────────────────────────────────────
    if (G::showStructureESP) {
        RECT scr2{};
        GetClientRect(g_hWnd, &scr2);
        int ssw = scr2.right, ssh = scr2.bottom;
        if (ssw > 0 && ssh > 0) {
            ImDrawList* sdl = ImGui::GetBackgroundDrawList();
            std::vector<StructureESP> strucs;
            {
                std::lock_guard<std::mutex> lk(G::mtx);
                strucs = G::structures;
            }

            for (const auto& s : strucs) {
                float ssx, ssy;
                if (!SafeW2S(s.pos, ssx, ssy)) continue;
                if (ssx < -600.f || ssx >(float)ssw + 600.f ||
                    ssy < -600.f || ssy >(float)ssh + 600.f) continue;

                float distM = s.dist * 0.01f;

                bool isEnemy = (G::localTeam >= 0) && ((int)s.teamId != G::localTeam);
                ImU32 scol;
                if (s.teamId == 0 && isEnemy)  scol = IM_COL32(50, 150, 255, 220);  // Warden - AZUL
                else if (s.teamId == 0)              scol = IM_COL32(70, 130, 200, 190);
                else if (s.teamId == 1 && isEnemy)  scol = IM_COL32(50, 255, 100, 220);  // Colonial - VERDE
                else if (s.teamId == 1)              scol = IM_COL32(80, 200, 100, 190);
                else                                 scol = IM_COL32(200, 200, 200, 175);

                ImU32 shad = IM_COL32(0, 0, 0, 210);

                float h = 5.f;
                sdl->AddRect({ ssx - h, ssy - h }, { ssx + h, ssy + h }, scol, 0.f, 0, 1.4f);

                if (s.maxHealth > 0) {
                    float frac = (float)s.health / (float)s.maxHealth;
                    if (frac < 0.f) frac = 0.f;
                    if (frac > 1.f) frac = 1.f;
                    float bw = 44.f, bh = 4.f;
                    float bx = ssx - bw * 0.5f, by = ssy - 20.f;
                    sdl->AddRectFilled({ bx, by }, { bx + bw, by + bh }, IM_COL32(20, 20, 20, 180), 1.5f);
                    ImU32 hpc = (frac > 0.6f) ? IM_COL32(70, 210, 70, 255)
                        : (frac > 0.3f) ? IM_COL32(220, 190, 35, 255)
                        : IM_COL32(215, 50, 50, 255);
                    if (frac > 0.f)
                        sdl->AddRectFilled({ bx,by }, { bx + bw * frac,by + bh }, hpc, 1.5f);
                    sdl->AddRect({ bx,by }, { bx + bw,by + bh }, IM_COL32(0, 0, 0, 100), 1.5f);
                }

                const char* teamTag = (s.teamId == 0) ? "W" : (s.teamId == 1) ? "C" : "?";
                char tag[128];
                snprintf(tag, sizeof(tag), "[%s] %s\n%.0fm  %d/%d hp",
                    teamTag, s.clsName, distM, s.health, s.maxHealth);
                float tx = ssx + 8.f, ty = ssy - 9.f;
                sdl->AddText({ tx + 1.f, ty + 1.f }, shad, tag);
                sdl->AddText({ tx,     ty }, scol, tag);
            }
        }
    }

    // ─────────────────────────────────────────────────────────
    // Autoaim visual indicator
    // ─────────────────────────────────────────────────────────
    if (G::enableAutoaim && G::autoaimActive && IsValidPtr(G::autoaimTargetAddr)) {
        FVec3 targetPos{};
        if (GetAutoaimTargetScreenPos(G::autoaimTargetAddr, targetPos)) {
            ImDrawList* aimDL = ImGui::GetBackgroundDrawList();
            ImU32 aimColor = IM_COL32(255, 100, 0, 255);

            // Dibujar círculo alrededor del objetivo
            aimDL->AddCircle({ targetPos.x, targetPos.y }, 12.f, aimColor, 0, 2.f);
            aimDL->AddCircle({ targetPos.x, targetPos.y }, 15.f, aimColor, 0, 1.f);

            // Mostrar "TRACKING"
            aimDL->AddText({ targetPos.x + 20.f, targetPos.y - 10.f }, aimColor, "TRACKING");
        }
    }

    // ─────────────────────────────────────────────────────────
    // ImGui Menu
    // ─────────────────────────────────────────────────────────
    if (G::showMenu) {
        ImGui::SetNextWindowSize({ 500, 850 }, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({ 60,  60 }, ImGuiCond_FirstUseEver);

        if (ImGui::Begin("## foxhole_esp", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::TextColored({ 0.45f,0.75f,1.f,1.f }, "Foxhole ESP - Simplified");
            ImGui::TextDisabled("INSERT = toggle menu");
            ImGui::Separator();

            ImGui::SeparatorText("Player ESP");
            ImGui::Checkbox("Enable ESP", &G::espEnabled);
            ImGui::Checkbox("Enemy only", &G::enemyOnly);
            ImGui::Checkbox("Show HP bar", &G::showHPBar);
            ImGui::Checkbox("Show distance", &G::showDist);
            ImGui::Checkbox("Show lines", &G::showLines);
            ImGui::SliderFloat("Max distance (m)", &G::maxDist, 5000.f, 500000.f, "%.0f");

            ImGui::Spacing();
            ImGui::SeparatorText("Skeleton Bones");
            ImGui::Checkbox("Show bones##bones", &G::showBones);
            ImGui::TextDisabled("(Simplified: head, torso, legs only)");

            float r = ((G::boneColor >> 0) & 0xFF) / 255.f;
            float g = ((G::boneColor >> 8) & 0xFF) / 255.f;
            float b = ((G::boneColor >> 16) & 0xFF) / 255.f;
            ImVec4 boneColorVec(r, g, b, 1.f);
            if (ImGui::ColorEdit4("Bone color##bc", (float*)&boneColorVec, ImGuiColorEditFlags_NoAlpha)) {
                G::boneColor = IM_COL32(
                    (uint8_t)(boneColorVec.x * 255),
                    (uint8_t)(boneColorVec.y * 255),
                    (uint8_t)(boneColorVec.z * 255),
                    200
                );
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Vehicle ESP");
            ImGui::Checkbox("Enable vehicle ESP##veh", &G::showVehicleESP);
            ImGui::SameLine();
            ImGui::Checkbox("Enemy only##veh", &G::vehicleEnemyOnly);
            ImGui::SliderFloat("Max dist##veh", &G::vehicleMaxDist, 10.f, 2000.f, "%.0f m");

            ImGui::Spacing();
            ImGui::SeparatorText("Vehicle Proximity Alert");
            ImGui::Checkbox("Enable proximity detection##prox", &G::enableVehicleProximity);
            ImGui::SliderFloat("Proximity distance (m)##prox", &G::vehicleProximityDist, 100.f, 5000.f, "%.0f");
            ImGui::TextDisabled("(Updates every 100ms)");

            int proximityCount;
            {
                std::lock_guard<std::mutex> lk(G::mtx);
                proximityCount = (int)G::nearbyVehicles.size();
            }
            ImGui::Text("Nearby vehicles: %d", proximityCount);

            if (proximityCount > 0) {
                ImGui::Spacing();
                ImGui::SeparatorText("Nearby Vehicles List");
                {
                    std::lock_guard<std::mutex> lk(G::mtx);
                    for (size_t i = 0; i < G::nearbyVehicles.size(); ++i) {
                        const auto& nv = G::nearbyVehicles[i];
                        float distM = nv.dist * 0.01f;
                        const char* teamTag = (nv.teamId == 0) ? "Warden" : (nv.teamId == 1) ? "Colonial" : "Unknown";
                        ImGui::Text("[%d] %s - %s (%.1f m, HP: %d)",
                            (int)i + 1, nv.clsName, teamTag, distM, nv.health);
                    }
                }
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Structure ESP");
            ImGui::Checkbox("Enable structure ESP##str", &G::showStructureESP);
            ImGui::SameLine();
            ImGui::Checkbox("Enemy only##str", &G::structureEnemyOnly);
            ImGui::SliderFloat("Max dist##str", &G::structureMaxDist, 10.f, 1000.f, "%.0f m");

            ImGui::Spacing();
            ImGui::SeparatorText("Autoaim");
            ImGui::Checkbox("Enable autoaim##aim", &G::enableAutoaim);
            ImGui::SameLine();
            ImGui::Checkbox("Enemy only##aim", &G::autoaimEnemyOnly);

            // Bone selection
            ImGui::Text("Target bone:");
            ImGui::SameLine();
            if (ImGui::RadioButton("Head##aimhead", &G::autoaimBoneTarget, 0)) {}
            ImGui::SameLine();
            if (ImGui::RadioButton("Central##aimcentral", &G::autoaimBoneTarget, 1)) {}

            // Activation key
            ImGui::Text("Activation key:");
            ImGui::SameLine();
            const char* keyNames[] = {
                "None", "LMB", "RMB", "MMB", "X1", "X2"
            };
            int keyIdx = 0;
            if (G::autoaimActivationKey == VK_LBUTTON) keyIdx = 1;
            else if (G::autoaimActivationKey == VK_RBUTTON) keyIdx = 2;
            else if (G::autoaimActivationKey == VK_MBUTTON) keyIdx = 3;
            else if (G::autoaimActivationKey == VK_XBUTTON1) keyIdx = 4;
            else if (G::autoaimActivationKey == VK_XBUTTON2) keyIdx = 5;

            if (ImGui::Combo("##aimkey", &keyIdx, keyNames, IM_ARRAYSIZE(keyNames))) {
                switch (keyIdx) {
                case 0: G::autoaimActivationKey = 0; break;
                case 1: G::autoaimActivationKey = VK_LBUTTON; break;
                case 2: G::autoaimActivationKey = VK_RBUTTON; break;
                case 3: G::autoaimActivationKey = VK_MBUTTON; break;
                case 4: G::autoaimActivationKey = VK_XBUTTON1; break;
                case 5: G::autoaimActivationKey = VK_XBUTTON2; break;
                }
            }

            // Smoothing slider
            ImGui::SliderFloat("Smoothing##aim", &G::autoaimSmoothing, 0.f, 1.f, "%.2f");
            ImGui::TextDisabled("(0.0 = instant, 1.0 = very smooth)");

            ImGui::Spacing();
            ImGui::SeparatorText("Status");
            int visCount;
            {
                std::lock_guard<std::mutex> lk(G::mtx);
                visCount = (int)G::players.size();
            }
            ImGui::Text("Visible players : %d", visCount);
            ImGui::Text("Local team      : %s",
                G::localTeam == 0 ? "Warden" :
                G::localTeam == 1 ? "Colonial" : "Unknown");

            ImGui::Spacing();
            if (ImGui::Button("Unload DLL", { 120, 0 })) {
                g_unloadRequested = true;
            }
        }
        ImGui::End();
    }

    ImGui::Render();
    if (g_pCtx && g_pRTV) {
        g_pCtx->OMSetRenderTargets(1, &g_pRTV, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }

    return g_oPresent(sc, sync, flags);
}

// ═══════════════════════════════════════════════════════════════
// Hook Setup
// ═══════════════════════════════════════════════════════════════
static bool g_hookAttempted = false;
static bool g_hookSuccess = false;

static DWORD WINAPI HookPresentThreaded(LPVOID)
{
    Sleep(3000);

    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcA;
    wc.lpszClassName = "FH_HOOK_CLS";
    RegisterClassExA(&wc);

    HWND dummy = CreateWindowExA(0, "FH_HOOK_CLS", "", WS_OVERLAPPEDWINDOW,
        0, 0, 2, 2, nullptr, nullptr, nullptr, nullptr);
    if (!dummy) return 0;

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummy;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* dSC = nullptr;
    ID3D11Device* dDev = nullptr;
    D3D_FEATURE_LEVEL fl;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &dSC, &dDev, &fl, nullptr);

    if (FAILED(hr)) {
        DestroyWindow(dummy);
        return 0;
    }

    g_scVTable = *reinterpret_cast<void***>(dSC);

    DWORD old;
    VirtualProtect(g_scVTable, sizeof(void*) * 19, PAGE_EXECUTE_READWRITE, &old);

    g_oPresent = reinterpret_cast<FnPresent>(g_scVTable[8]);
    g_scVTable[8] = reinterpret_cast<void*>(HookedPresent);

    g_oResizeBuffers = reinterpret_cast<FnResizeBuffers>(g_scVTable[13]);
    g_scVTable[13] = reinterpret_cast<void*>(HookedResizeBuffers);

    VirtualProtect(g_scVTable, sizeof(void*) * 19, old, &old);

    dSC->Release();
    dDev->Release();
    DestroyWindow(dummy);
    UnregisterClassA("FH_HOOK_CLS", nullptr);

    g_hookSuccess = true;
    return 1;
}

static bool HookPresent()
{
    if (g_hookAttempted) return g_hookSuccess;
    g_hookAttempted = true;

    HANDLE hThread = CreateThread(nullptr, 0, HookPresentThreaded, nullptr, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
        return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════
// Worker Thread
// ═══════════════════════════════════════════════════════════════
static DWORD WINAPI WorkerThread(LPVOID)
{
    G::base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    printf("[ESP] Worker started. INSERT = toggle menu.\n");
    printf("[ESP] Vehicle Proximity Detection = enabled (100ms intervals)\n");
    printf("[ESP] Vehicle Class Auto-Caching = enabled (5s intervals)\n");
    printf("[ESP] Skeleton Bones = enabled (simplified humanoid structure)\n");
    printf("[ESP] Vehicle detection = BALANCED (keyword-based + inheritance + blacklist)\n");
    printf("[ESP] AUTOAIM = enabled (Head/Central targeting with smoothing)\n");

    uint32_t lastProximityScan = 0;
    uint32_t lastVehicleClassCache = 0;

    while (!g_doUnload) {
        uint32_t now = static_cast<uint32_t>(GetTickCount());

        if (G::espEnabled) SafeUpdateESP();
        if (G::showVehicleESP) SafeUpdateVehicleESP();
        if (G::showStructureESP) SafeUpdateStructureESP();

        // Vehicle proximity detection every 100ms
        if (G::enableVehicleProximity && (now - lastProximityScan >= 100)) {
            SafeUpdateVehicleProximity();
            lastProximityScan = now;
        }

        // Auto vehicle class caching every 5 seconds
        if ((now - lastVehicleClassCache >= 5000) || lastVehicleClassCache == 0) {
            AutoCacheVehicleClasses();
            lastVehicleClassCache = now;
        }

        Sleep(50);
    }

    Sleep(1000);

    if (g_scVTable) {
        if (g_scVTable[8] == reinterpret_cast<void*>(HookedPresent)) {
            DWORD old;
            VirtualProtect(g_scVTable, sizeof(void*) * 19, PAGE_EXECUTE_READWRITE, &old);
            g_scVTable[8] = reinterpret_cast<void*>(g_oPresent);
            g_scVTable[13] = reinterpret_cast<void*>(g_oResizeBuffers);
            VirtualProtect(g_scVTable, sizeof(void*) * 19, old, &old);
        }
        g_scVTable = nullptr;
    }

    SuspendThread(GetCurrentThread());
    return 0;
}

// ═══════════════════════════════════════════════════════════════
// DllMain
// ═══════════════════════════════════════════════════════════════
BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        AllocConsole();
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        SetConsoleTitleA("Foxhole ESP");

        printf("========================================\n");
        printf("[ESP] DLL INJECTED SUCCESSFULLY\n");
        printf("[ESP] Foxhole ESP - Simplified Skeleton Version\n");
        printf("[ESP] Vehicle Proximity Detection ENABLED\n");
        printf("[ESP] Vehicle Class Auto-Caching ENABLED (5s)\n");
        printf("[ESP] Skeleton Bones (Simplified) ENABLED\n");
        printf("[ESP] Vehicle Detection BALANCED\n");
        printf("[ESP] AUTOAIM WITH SMOOTHING ENABLED\n");
        printf("========================================\n");

        g_hSelf = hMod;
        HookPresent();
    }
    return TRUE;
}