// fortnite_emotes — third-person camera (the original's camera.cpp: SPEC §E.2 "Camera prop" /
// "Camera math").
//
// The camera is an invisible prop_dynamic (the chicken model) that the player's
// CPlayer_CameraServices::m_hViewEntity points at; the position follows the pawn at 110 units
// behind and 75 up, pulled in to the first wall seen from the pawn's eye.
//
// Differences to the original: the view angles come from the schema field v_angle (which keeps
// updating while the pawn is frozen) with s2sdk::GetClientEyeAngles as a fallback, and the traces
// are s2sdk::TraceLine (mask + ignored entity) instead of a gamedata-resolved UTIL_TraceShape.
// s2sdk::TraceHull cannot be used: it fills the VScript table with "start"/"end" while the engine
// reads "startpos"/"endpos", so every hull trace fails ("Didnt supply startpos, endpos, min, and
// max to Script TraceHull call", s2sdk 2.18.2).
#include "fe.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace {

constexpr float kCameraDistance = 110.0f;
constexpr float kCameraOffsetZ = 75.0f;
constexpr float kCameraNWUOffsetZ = 90.0f;
// Wall safety: the camera is swept from the pawn's eye as five parallel lines (the centre and
// kCameraProbeRadius left, right, up and down of it) and stops kCameraProbeRadius +
// kCameraProbeMargin before the first wall any of them meets. A camera within the client's near
// plane of a wall (or past it) shows what is behind the wall, which players used to look through
// walls while dancing.
constexpr float kCameraProbeRadius = 12.0f;
constexpr float kCameraProbeMargin = 2.0f;
constexpr float kCameraEyeHeight = 64.0f; // standing eye height, when the client eye is unknown
constexpr float kCameraTraceLerp = 0.3f; // CameraBlockObject on
constexpr float kCameraPlainLerp = 0.1f; // CameraBlockObject off

// The player trace mask of the original's @aim target (TraceFromEyes).
constexpr int32_t kCameraTraceMask = 0x1C300B;
// The camera sweep: CONTENTS_SOLID | BLOCK_LOS | WORLD_GEOMETRY | PHYSICS_PROP (interaction layers
// 0, 6, 14, 21; checked on the server: 0x1 stops at world brushes, 0x2 alone does not). No hitbox
// or player layers, so neither other players nor the dancer's own non-solid props (rig, clone and
// the camera itself) stop the camera.
constexpr int32_t kCameraWallMask = 0x204041;

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

int RandomPropSuffix()
{
	static std::mt19937 s_Rng{(unsigned)::time(nullptr)};
	static std::uniform_int_distribution<int> s_Dist(1000000, 9999998);
	return s_Dist(s_Rng);
}

// v_angle keeps updating while the pawn is frozen, so the camera orbits with the mouse.
// iSlot may be -1 (the owner of the pawn could not be resolved): s2sdk's client getters log a
// warning for such a slot, so they are only tried with a valid one.
//
// CBasePlayerPawn::v_angle is read through a cached offset and the raw vector getter: this runs
// once per frame per dancing player (OnFrame -> UpdateCamera -> CachedSafeCameraPosition, and
// CalculatePositionInFront), and the typed GetVectorField built the class+field schema keys on
// every call. A null pointer (invalid/stale handle, no entity system) or an all-zero angle falls
// through to the same eye-angle fallback as before.
plg::vec3 GetViewAngles(int iSlot, int iPawnHandle)
{
	static const int s_iVAngle = Offset("CBasePlayerPawn", "v_angle");
	if (IsValidHandle(iPawnHandle) && s_iVAngle >= 0 && __s2sdk_EntHandleToEntPointer && __s2sdk_GetEntDataVector3D2)
	{
		void* pEntity = nullptr;
		try { pEntity = s2sdk::EntHandleToEntPointer(iPawnHandle); }
		catch (...) { pEntity = nullptr; }
		if (pEntity)
		{
			plg::vec3 vAngle{};
			try { vAngle = s2sdk::GetEntDataVector3D2(pEntity, s_iVAngle); }
			catch (...) { vAngle = plg::vec3{}; }
			if (vAngle.x != 0.0f || vAngle.y != 0.0f || vAngle.z != 0.0f) return vAngle;
		}
	}

	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS || !__s2sdk_GetClientEyeAngles) return plg::vec3{};
	try { return s2sdk::GetClientEyeAngles(iSlot); }
	catch (...) { return plg::vec3{}; }
}

// The sweep is the expensive part of the per-frame camera update (s2sdk's traces run through the
// engine's VScript VM, table in and table out). Its result only depends on the pawn's origin and
// yaw, so it is cached per slot while those stay the same (a dancing player stands still most of
// the time) and refreshed at least every kTraceCacheSeconds in case the world around him changes.
constexpr double kTraceCacheSeconds = 2.0;

struct TraceCache
{
	bool bValid = false;
	double flStamp = 0.0;
	plg::vec3 vecOrigin{};
	float flYaw = 0.0f;
	float flSafe = 0.0f;
};
TraceCache g_TraceCache[FE_MAX_PLAYERS];

float Distance(const plg::vec3& a, const plg::vec3& b)
{
	const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

plg::vec3 EyePosition(int iSlot, int iPawnHandle)
{
	if (iSlot >= 0 && iSlot < FE_MAX_PLAYERS && __s2sdk_GetClientEyePosition)
	{
		try { return s2sdk::GetClientEyePosition(iSlot); }
		catch (...) {}
	}
	plg::vec3 vecOrigin{};
	if (__s2sdk_GetEntityAbsOrigin)
	{
		try { vecOrigin = s2sdk::GetEntityAbsOrigin(iPawnHandle); }
		catch (...) {}
	}
	return plg::vec3{vecOrigin.x, vecOrigin.y, vecOrigin.z + kCameraEyeHeight};
}

// The camera's way out of the head: from the eye towards the spot kCameraDistance behind the view
// yaw, flHeight above the feet.
struct CameraRay
{
	plg::vec3 vecEye{};
	plg::vec3 vecDir{}; // unit length, or zero when the target sits on the eye
	float flLength = 0.0f;
};

CameraRay CameraRayFor(int iSlot, int iPawnHandle, float flHeight)
{
	CameraRay ray;
	ray.vecEye = EyePosition(iSlot, iPawnHandle);
	const plg::vec3 vecTarget = CalculatePositionInFront(iPawnHandle, -kCameraDistance, flHeight);
	ray.flLength = Distance(ray.vecEye, vecTarget);
	if (ray.flLength >= 0.001f)
	{
		const float flInv = 1.0f / ray.flLength;
		ray.vecDir = plg::vec3{(vecTarget.x - ray.vecEye.x) * flInv, (vecTarget.y - ray.vecEye.y) * flInv,
							   (vecTarget.z - ray.vecEye.z) * flInv};
	}
	return ray;
}

plg::vec3 PointOnRay(const CameraRay& ray, float flDistance)
{
	return plg::vec3{ray.vecEye.x + ray.vecDir.x * flDistance, ray.vecEye.y + ray.vecDir.y * flDistance,
					 ray.vecEye.z + ray.vecDir.z * flDistance};
}

// How far along the ray the camera may go without touching a wall. There is no forced minimum: if
// the player stands against a wall the camera stays at the eye instead of being pushed into the
// wall. A trace that cannot run, or starts inside a wall, keeps the camera at the eye as well.
float SweepCameraDistance(int iPawnHandle, const CameraRay& ray)
{
	if (!__s2sdk_TraceLine || ray.flLength < 0.001f) return 0.0f;

	// two unit vectors across the ray: sideways in the horizontal plane, and up/down
	plg::vec3 vecSide{-ray.vecDir.y, ray.vecDir.x, 0.0f};
	float flSide = sqrtf(vecSide.x * vecSide.x + vecSide.y * vecSide.y);
	if (flSide < 0.001f)
	{
		vecSide = plg::vec3{1.0f, 0.0f, 0.0f};
		flSide = 1.0f;
	}
	vecSide = plg::vec3{vecSide.x / flSide, vecSide.y / flSide, 0.0f};
	const plg::vec3 vecUp{ray.vecDir.y * vecSide.z - ray.vecDir.z * vecSide.y,
						  ray.vecDir.z * vecSide.x - ray.vecDir.x * vecSide.z,
						  ray.vecDir.x * vecSide.y - ray.vecDir.y * vecSide.x};

	const float r = kCameraProbeRadius;
	const plg::vec3 vecOffsets[] = {{0.0f, 0.0f, 0.0f},
									{vecSide.x * r, vecSide.y * r, 0.0f},
									{-vecSide.x * r, -vecSide.y * r, 0.0f},
									{vecUp.x * r, vecUp.y * r, vecUp.z * r},
									{-vecUp.x * r, -vecUp.y * r, -vecUp.z * r}};

	double flFree = 1.0;
	for (const plg::vec3& o : vecOffsets)
	{
		const plg::vec3 vecStart{ray.vecEye.x + o.x, ray.vecEye.y + o.y, ray.vecEye.z + o.z};
		const plg::vec3 vecEnd{vecStart.x + ray.vecDir.x * ray.flLength, vecStart.y + ray.vecDir.y * ray.flLength,
							   vecStart.z + ray.vecDir.z * ray.flLength};
		plg::vec3 vecHit{};
		double flFraction = -1.0; // s2sdk leaves the outputs alone when the ignored pawn is gone
		bool bHit = false, bStartSolid = false;
		int32_t iEntHit = 0;
		try
		{
			s2sdk::TraceLine(vecStart, vecEnd, kCameraWallMask, iPawnHandle, vecHit, flFraction, bHit, iEntHit,
							 bStartSolid);
		}
		catch (...)
		{
			return 0.0f;
		}
		if (flFraction < 0.0 || bStartSolid) return 0.0f;
		if (bHit && flFraction < flFree) flFree = flFraction;
	}

	if (flFree >= 1.0) return ray.flLength;
	const float flSafe = ray.flLength * (float)flFree - kCameraProbeRadius - kCameraProbeMargin;
	return flSafe > 0.0f ? flSafe : 0.0f;
}

// The camera spot for a teleport: kCameraDistance behind the view yaw, flHeight above the feet,
// pulled in to the first wall seen from the eye.
plg::vec3 SafeCameraPosition(int iSlot, int iPawnHandle, float flHeight)
{
	const CameraRay ray = CameraRayFor(iSlot, iPawnHandle, flHeight);
	return PointOnRay(ray, SweepCameraDistance(iPawnHandle, ray));
}

// SweepCameraDistance for the per-frame update, served from the per-slot cache while the pawn's
// origin and yaw stay the same.
float CachedSafeCameraDistance(int iSlot, int iPawnHandle, const CameraRay& ray)
{
	const plg::vec3 vecOrigin = s2sdk::GetClientAbsOrigin(iSlot);
	const float flYaw = GetViewAngles(iSlot, iPawnHandle).y;

	TraceCache& cache = g_TraceCache[iSlot];
	const double flNow = FE_Now();
	if (cache.bValid && flNow - cache.flStamp < kTraceCacheSeconds && cache.flYaw == flYaw
		&& cache.vecOrigin.x == vecOrigin.x && cache.vecOrigin.y == vecOrigin.y && cache.vecOrigin.z == vecOrigin.z)
		return cache.flSafe;

	cache.bValid = true;
	cache.flStamp = flNow;
	cache.vecOrigin = vecOrigin;
	cache.flYaw = flYaw;
	cache.flSafe = SweepCameraDistance(iPawnHandle, ray);
	return cache.flSafe;
}

// The camera services component class of this build ("CPlayer_CameraServices"; some builds use the
// CCS spelling). The lookup builds s2sdk's per-class schema table, so it only runs with a live map
// (never on the map-start path - see ResetCam) and only on demand, i.e. when a dance is about to
// show a camera. A positive result is kept; a miss is retried on the next demand (and logged once)
// so a lookup that ran before the engine's class info was usable cannot disable the camera for the
// whole session.
const char* CameraServicesClass()
{
	static const char* s_szClass = nullptr;
	static bool s_bLogged = false;
	if (s_szClass) return s_szClass;
	if (!pisex::IsGameReady()) return nullptr;

	if (Offset("CPlayer_CameraServices", "m_hViewEntity") >= 0) s_szClass = "CPlayer_CameraServices";
	else if (Offset("CCSPlayer_CameraServices", "m_hViewEntity") >= 0) s_szClass = "CCSPlayer_CameraServices";
	else if (!s_bLogged)
	{
		s_bLogged = true;
		LogWarning("camera services class not found in the schema - the third-person camera is disabled");
	}
	return s_szClass;
}

void* CameraServicesPtr(int iPawnHandle)
{
	if (!IsValidHandle(iPawnHandle)) return nullptr; // the pawn handle may be negative (serial)
	// m_pCameraServices is a stored-pointer ("ptr") field: the object is the pointer stored in
	// the field (GetComponentPointer), not the field address GetComponentPtr returns.
	void* p = GetComponentPointer(iPawnHandle, "CBasePlayerPawn", "m_pCameraServices");
	if (!p) p = GetComponentPointer(iPawnHandle, "CCSPlayerPawnBase", "m_pCameraServices");
	if (!p) p = GetComponentPointer(iPawnHandle, "CCSPlayerPawn", "m_pCameraServices");
	return p;
}

} // namespace

bool HasCameraServices(int iPawnHandle)
{
	return CameraServicesPtr(iPawnHandle) != nullptr;
}

bool InitCameraTrace()
{
	// Nothing to resolve any more: the traces run through s2sdk and only need the masks, which are
	// constants of this build.
	const bool bOk = __s2sdk_TraceLine != nullptr;
	if (!bOk) LogWarning("[camera] s2sdk TraceLine is missing, the emote camera stays at the eye");
	return bOk;
}

plg::vec3 CalculatePositionInFront(int iPawnHandle, float offsetXy, float offsetZ)
{
	// A pawn's entity index is NOT its owner's slot + 1 (that only holds for controllers), so the
	// slot comes from the pawn's owner - otherwise another player's position would be read here.
	const int iSlot = pisex::GetOwnerSlot(IndexOf(iPawnHandle));
	const plg::vec3 vecOrigin = (iSlot >= 0 && iSlot < FE_MAX_PLAYERS && __s2sdk_GetClientAbsOrigin)
		? s2sdk::GetClientAbsOrigin(iSlot)
		: (__s2sdk_GetEntityAbsOrigin ? s2sdk::GetEntityAbsOrigin(iPawnHandle) : plg::vec3{});
	const float flYaw = GetViewAngles(iSlot, iPawnHandle).y * kDeg2Rad;

	return plg::vec3{vecOrigin.x + offsetXy * cosf(flYaw), vecOrigin.y + offsetXy * sinf(flYaw),
					 vecOrigin.z + offsetZ};
}

int SetCam(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return 0;

	const int iPawn = GetAlivePawn(iSlot);
	if (iPawn <= 0) return 0;
	const int iPawnHandle = Handle(iPawn);
	if (!CameraServicesPtr(iPawnHandle)) return 0; // no camera without camera services

	// CreateEntityByName answers a HANDLE (INVALID_EHANDLE_INDEX when it failed), not an index
	int iCamera = 0;
	try { iCamera = s2sdk::CreateEntityByName("prop_dynamic"); }
	catch (...) { return 0; }
	if (!IsValidHandle(iCamera))
	{
		LogWarning("[camera] failed to create the camera prop for slot %d", iSlot);
		return 0;
	}

	char szName[32];
	std::snprintf(szName, sizeof(szName), "cameraProp_%d", RandomPropSuffix());

	const plg::vec3 angView = GetViewAngles(iSlot, iPawnHandle);
	const plg::vec3 vecFirst = SafeCameraPosition(iSlot, iPawnHandle, kCameraOffsetZ);
	try { s2sdk::TeleportEntity(iCamera, vecFirst, angView, plg::vec3{0, 0, 0}); }
	catch (...) {}

	PrepareProp(iCamera, 0);
	try { s2sdk::SetEntityModel(iCamera, plg::string("models/chicken/chicken.vmdl")); }
	catch (...) {}

	// invisible: EF_NOSHADOW + alpha 0
	// EF_NOSHADOW is 0x10. 0x20 (1 << 5) is EF_NODRAW (see BlockerPasses kEfNoDraw), and a NODRAW
	// entity is not transmitted: the dancer's client never got the camera prop and kept first person.
	constexpr int64_t EF_NOSHADOW = 1ll << 4;
	SetIntField(iCamera, "CBaseEntity", "m_fEffects", GetIntField(iCamera, "CBaseEntity", "m_fEffects") | EF_NOSHADOW, true);
	SetEntityAlpha(iCamera, 0);

	// The original spawns the prop here (SPEC §E.2 "SetModel -> DispatchSpawn -> Teleport"), with
	// the name as the "targetname" spawn keyvalue. A prop that was only created is
	// not spawned into the world, the client cannot resolve the m_hViewEntity handle, and the dancer
	// keeps the first-person view. Same keyvalues as CreateProp (emote.cpp).
	plg::vector<plg::string> spawnKeys{plg::string("model"), plg::string("targetname")};
	plg::vector<plg::any> spawnValues{plg::any(plg::string("models/chicken/chicken.vmdl")), plg::any(plg::string(szName))};
	try { s2sdk::DispatchSpawn2(iCamera, spawnKeys, spawnValues); }
	catch (...) {}

	const plg::vec3 vecOrigin = s2sdk::GetClientAbsOrigin(iSlot);
	try { s2sdk::TeleportEntity(iCamera, vecOrigin, angView, plg::vec3{0, 0, 0}); }
	catch (...) {}

	// third-person position and the view entity, only while this emote is still the current one
	PlayerState& state = g_States[iSlot];
	const uint32_t nGeneration = state.Generation;
	pisex::NextFrame([iSlot, nGeneration, iCamera]()
	{
		if (!IsDancing(iSlot) || g_States[iSlot].Generation != nGeneration)
			return;
		if (!ValidHandle(iCamera)) // the camera prop was removed in the meantime
			return;

		const int iNow = GetAlivePawn(iSlot);
		if (iNow <= 0) return;
		const int iNowHandle = Handle(iNow);
		void* pCameraServices = CameraServicesPtr(iNowHandle);
		if (!pCameraServices) return;

		const plg::vec3 angNow = GetViewAngles(iSlot, iNowHandle);
		const plg::vec3 vecBehind = SafeCameraPosition(iSlot, iNowHandle, kCameraNWUOffsetZ);
		try { s2sdk::TeleportEntity(iCamera, vecBehind, angNow, plg::vec3{0, 0, 0}); }
		catch (...) {}

		// raw write: the typed setter dereferences a schema table type pointer that can be invalid
		// (see SetComponentEntityRaw); the view entity is a 4-byte CHandle<CBaseEntity>
		const char* szClass = CameraServicesClass();
		const bool bWritten = szClass && SetComponentEntityRaw(pCameraServices, szClass, "m_hViewEntity", iCamera, true);
		if (bWritten)
			g_States[iSlot].CameraSet = true; // only now is there a camera that needs restoring
		// the pawn's camera-services field is what the clients rebuild the view from
		StateChanged(iNowHandle, "CBasePlayerPawn", "m_pCameraServices");
	});

	return iCamera;
}

void ResetCam(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;
	if (!pisex::g_pUtils) return;

	// Only a dance creates a camera (SetCam) and only a shown camera needs restoring. StopEmote is
	// forced for every slot by player_spawn/player_death/player_disconnect, so without this gate the
	// write below ran on the first frame after a map load - the boot changelevel spawns the bots -
	// and was the first schema access to the camera-services class, at exactly the moment the engine
	// builds its class info: s2sdk's table then carried CSchemaType* == -1 and SetEntSchemaEnt2's
	// schema::IsAtomicType dereferenced it (SIGSEGV at 0x17; see
	// SetComponentEntityRaw in fe_entity.cpp for the type-dereference-free write used below).
	//
	// The flag is consumed here, when the restore is queued - not in the callback: the stop can run
	// again (a death right after a round event) and that must not cancel a restore already on its
	// way. A later dance sets the flag again and queues its own camera write after this one.
	if (!g_States[iSlot].CameraSet) return;
	g_States[iSlot].CameraSet = false;

	// deliberately not generation-guarded: a stop must always restore first person
	pisex::g_pUtils->NextFrame([iSlot]()
	{
		// Re-validated at fire time: this callback can run a frame (or a map) later, when the map is
		// gone, the player has disconnected or the pawn has died with its camera services.
		// IsClientReady (silent): a live map, the slot in range and its client/controller present.
		if (!pisex::IsClientReady(iSlot)) return;

		const int iPawn = GetAlivePawn(iSlot);
		const int iPawnHandle = iPawn > 0 ? Handle(iPawn) : 0;
		void* pCameraServices = IsValidHandle(iPawnHandle) ? CameraServicesPtr(iPawnHandle) : nullptr;
		if (!pCameraServices) return; // the pawn is gone; its view entity went with it

		const char* szClass = CameraServicesClass(); // a shown camera proves the class resolves
		if (szClass) SetComponentEntityRaw(pCameraServices, szClass, "m_hViewEntity", -1, true);
		StateChanged(iPawnHandle, "CBasePlayerPawn", "m_pCameraServices");
	});
}

void UpdateCamera(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS || !g_Settings.CameraMoveable) return;

	PlayerState& state = g_States[iSlot];
	if (!state.IsDancing || !IsValidHandle(state.Camera)) return;
	// the prop can be gone (engine cleanup, a map change we did not see): drop it instead of
	// hammering s2sdk with an invalid handle every frame
	if (!ValidHandle(state.Camera))
	{
		state.Camera = 0;
		return;
	}

	const int iPawn = GetAlivePawn(iSlot);
	if (iPawn <= 0) return;
	const int iPawnHandle = Handle(iPawn);
	if (!IsValidHandle(iPawnHandle)) return;

	// Walls are always checked, whatever CameraBlockObject says: an unchecked camera is a wallhack.
	// CameraBlockObject only keeps choosing the smoothing speed it chose before.
	//
	// The camera always sits on the swept ray: the direction follows the view at once and only the
	// distance is smoothed. Moving out is smoothed, moving in to a nearer wall is not. (Smoothing
	// the position itself moved the camera along a chord between two safe points, and while the
	// camera orbited that chord cut through corners.)
	const CameraRay ray = CameraRayFor(iSlot, iPawnHandle, kCameraOffsetZ);
	const float flSafe = CachedSafeCameraDistance(iSlot, iPawnHandle, ray);
	float flCurrent = flSafe;
	if (__s2sdk_GetEntityAbsOrigin)
	{
		try { flCurrent = Distance(ray.vecEye, s2sdk::GetEntityAbsOrigin(state.Camera)); }
		catch (...) {}
	}

	const float flLerp = g_Settings.CameraBlockObject ? kCameraTraceLerp : kCameraPlainLerp;
	float flDistance = flCurrent + (flSafe - flCurrent) * flLerp;
	if (flDistance > flSafe) flDistance = flSafe;
	const plg::vec3 vecPosition = PointOnRay(ray, flDistance);

	const plg::vec3 angView = GetViewAngles(iSlot, iPawnHandle);
	try { s2sdk::TeleportEntity(state.Camera, vecPosition, angView, plg::vec3{0, 0, 0}); }
	catch (...) {}
}

void UpdateRigFollow(int iSlot)
{
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS) return;

	PlayerState& state = g_States[iSlot];
	if (!state.IsDancing || !IsValidHandle(state.Rig)) return;
	if (!ValidHandle(state.Rig))
	{
		state.Rig = 0;
		return;
	}

	const int iPawn = GetAlivePawn(iSlot);
	if (iPawn <= 0) return;

	try
	{
		s2sdk::TeleportEntity(state.Rig, s2sdk::GetClientAbsOrigin(iSlot), s2sdk::GetClientAbsAngles(iSlot),
							  plg::vec3{0, 0, 0});
	}
	catch (...)
	{
	}
}

int TraceFromEyes(int iPawnHandle, float flDistance)
{
	if (!IsValidHandle(iPawnHandle) || !__s2sdk_TraceLine) return 0;

	// the slot of the pawn's *owner* (a pawn index is not slot + 1); without a valid slot there is
	// no eye position to trace from
	const int iSlot = pisex::GetOwnerSlot(IndexOf(iPawnHandle));
	if (iSlot < 0 || iSlot >= FE_MAX_PLAYERS || !__s2sdk_GetClientEyePosition) return 0;

	const plg::vec3 vecStart = s2sdk::GetClientEyePosition(iSlot);
	const plg::vec3 vecAngles = GetViewAngles(iSlot, iPawnHandle);
	const plg::vec3 vecForward = s2sdk::AnglesToVector(vecAngles);
	const plg::vec3 vecEnd{vecStart.x + vecForward.x * flDistance, vecStart.y + vecForward.y * flDistance,
						   vecStart.z + vecForward.z * flDistance};

	plg::vec3 vecHit{};
	double flFraction = 1.0;
	bool bHit = false, bStartSolid = false;
	int32_t iEntHit = 0;
	try
	{
		if (!s2sdk::TraceLine(vecStart, vecEnd, kCameraTraceMask, iPawnHandle, vecHit, flFraction, bHit, iEntHit,
							  bStartSolid))
			return 0;
	}
	catch (...)
	{
		return 0;
	}
	if (!bHit) return 0;

	// outEntHit is an entity handle (INVALID_EHANDLE_INDEX when nothing was hit); the world (index
	// 0) resolves to no target either
	return IndexOf(iEntHit);
}
