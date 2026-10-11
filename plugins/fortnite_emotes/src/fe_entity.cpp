// fortnite_emotes — the entity/schema helpers declared in fe.hpp.
//
// Every helper checks its s2sdk binding and swallows exceptions: a missing binding or an unknown
// field must degrade to "no value"/"no write" instead of taking the server down.
#include "fe.hpp"

#include <cstring>

namespace {

template <typename Fn, typename Fallback>
auto Guarded(Fn&& fn, Fallback&& fallback) -> decltype(fn())
{
	try
	{
		return fn();
	}
	catch (...)
	{
		return fallback();
	}
}

} // namespace

int Handle(int iIndex)
{
	if (iIndex <= 0 || !__s2sdk_EntIndexToEntHandle) return 0;
	const int iHandle = Guarded([&] { return s2sdk::EntIndexToEntHandle(iIndex); }, [] { return 0; });
	// s2sdk answers -1 for an index that has no entity; the handle itself may be negative (serial)
	return IsValidHandle(iHandle) ? iHandle : 0;
}

int IndexOf(int iHandle)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_EntHandleToEntIndex) return 0;
	const int iIndex = Guarded([&] { return s2sdk::EntHandleToEntIndex(iHandle); }, [] { return 0; });
	// s2sdk answers -1 for a stale handle; the world (index 0) is "no entity" here as well
	return iIndex > 0 ? iIndex : 0;
}

bool ValidHandle(int iHandle)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_IsValidEntHandle) return false;
	return Guarded([&] { return s2sdk::IsValidEntHandle(iHandle); }, [] { return false; });
}

int Offset(const char* szClass, const char* szField)
{
	return pisex::SchemaOffset(szClass, szField);
}

int ChainOffset(const char* szClass)
{
	// s2sdk's SafeNetworkStateChanged() reads a chain offset >= 0 as "the __m_pChainEntity of the
	// object is at entity + chainOffset" - a 0 from a missing binding would make it read the
	// object's vtable as a chainer. No binding (or no chainer) therefore means -1: the object
	// itself is the entity.
	if (!__s2sdk_GetSchemaChainOffset) return -1;
	const int iOffset = pisex::SchemaChainOffset(szClass);
	return iOffset > 0 ? iOffset : -1;
}

// ---------------------------------------------------------------------------------------------

int64_t GetIntField(int iHandle, const char* szClass, const char* szField, int64_t iDefault, int iElement)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_GetEntSchema) return iDefault;
	return Guarded(
		[&] { return s2sdk::GetEntSchema(iHandle, plg::string(szClass), plg::string(szField), iElement); },
		[&] { return iDefault; });
}

float GetFloatField(int iHandle, const char* szClass, const char* szField, float flDefault)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_GetEntSchemaFloat) return flDefault;
	return Guarded(
		[&] { return (float)s2sdk::GetEntSchemaFloat(iHandle, plg::string(szClass), plg::string(szField), 0); },
		[&] { return flDefault; });
}

bool GetBoolField(int iHandle, const char* szClass, const char* szField, bool bDefault)
{
	return GetIntField(iHandle, szClass, szField, bDefault ? 1 : 0) != 0;
}

std::string GetStringField(int iHandle, const char* szClass, const char* szField)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_GetEntSchemaString) return {};
	return Guarded(
		[&] {
			const plg::string s = s2sdk::GetEntSchemaString(iHandle, plg::string(szClass), plg::string(szField), 0);
			return std::string(s.data(), s.size());
		},
		[] { return std::string(); });
}

plg::vec3 GetVectorField(int iHandle, const char* szClass, const char* szField)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_GetEntSchemaVector3D) return {};
	return Guarded(
		[&] { return s2sdk::GetEntSchemaVector3D(iHandle, plg::string(szClass), plg::string(szField), 0); },
		[] { return plg::vec3{}; });
}

int GetEntityField(int iHandle, const char* szClass, const char* szField)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_GetEntSchemaEnt) return 0;
	const int iEnt = Guarded(
		[&] { return s2sdk::GetEntSchemaEnt(iHandle, plg::string(szClass), plg::string(szField), 0); },
		[] { return 0; });
	// a CHandle field read back: -1 (INVALID_EHANDLE_INDEX) means "no entity", like 0
	return IsValidHandle(iEnt) ? iEnt : 0;
}

void SetIntField(int iHandle, const char* szClass, const char* szField, int64_t iValue, bool bChangeState, int iElement)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_SetEntSchema) return;
	Guarded(
		[&] {
			s2sdk::SetEntSchema(iHandle, plg::string(szClass), plg::string(szField), iValue, bChangeState, iElement);
			return 0;
		},
		[] { return 0; });
}

void SetFloatField(int iHandle, const char* szClass, const char* szField, float flValue, bool bChangeState)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_SetEntSchemaFloat) return;
	Guarded(
		[&] {
			s2sdk::SetEntSchemaFloat(iHandle, plg::string(szClass), plg::string(szField), flValue, bChangeState, 0);
			return 0;
		},
		[] { return 0; });
}

void SetBoolField(int iHandle, const char* szClass, const char* szField, bool bValue, bool bChangeState)
{
	SetIntField(iHandle, szClass, szField, bValue ? 1 : 0, bChangeState);
}

void SetVectorField(int iHandle, const char* szClass, const char* szField, const plg::vec3& v, bool bChangeState)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_SetEntSchemaVector3D) return;
	Guarded(
		[&] {
			s2sdk::SetEntSchemaVector3D(iHandle, plg::string(szClass), plg::string(szField), v, bChangeState, 0);
			return 0;
		},
		[] { return 0; });
}

void SetEntityField(int iHandle, const char* szClass, const char* szField, int iValue, bool bChangeState)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_SetEntSchemaEnt) return;
	Guarded(
		[&] {
			s2sdk::SetEntSchemaEnt(iHandle, plg::string(szClass), plg::string(szField), iValue, bChangeState, 0);
			return 0;
		},
		[] { return 0; });
}

void StateChanged(int iHandle, const char* szClass, const char* szField)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_NetworkStateChanged) return;
	Guarded(
		[&] {
			s2sdk::NetworkStateChanged(iHandle, plg::string(szClass), plg::string(szField));
			return 0;
		},
		[] { return 0; });
}

// ---------------------------------------------------------------------------------------------

void AcceptInputString(int iHandle, const char* szInput, const std::string& sValue, int iActivatorHandle,
					   int iCallerHandle)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_AcceptEntityInput) return;
	if (__s2sdk_IsValidEntHandle && !s2sdk::IsValidEntHandle(iHandle)) return; // silent, no warning spam
	Guarded(
		[&] {
			// the activator/caller are handles as well (they may be negative): only a missing one
			// (0) is turned into s2sdk's "no entity" -1
			s2sdk::AcceptEntityInput(iHandle, plg::string(szInput),
									 IsValidHandle(iActivatorHandle) ? iActivatorHandle : -1,
									 IsValidHandle(iCallerHandle) ? iCallerHandle : -1,
									 plg::any(plg::string(sValue)), s2sdk::FieldType::String, 0);
			return 0;
		},
		[] { return 0; });
}

void* GetComponentPtr(int iHandle, const char* szOwnerClass, const char* szField)
{
	if (!IsValidHandle(iHandle) || !__s2sdk_GetEntSchemaPtr) return nullptr;
	return Guarded(
		[&] { return s2sdk::GetEntSchemaPtr(iHandle, plg::string(szOwnerClass), plg::string(szField), 0); },
		[] { return static_cast<void*>(nullptr); });
}

// The object a stored-pointer ("ptr" kind) field points at: m_pWeaponServices, m_pCameraServices,
// m_CBodyComponent. s2sdk's GetEntSchemaPtr returns the ADDRESS of the field (entity + offset),
// never the pointer it stores, so a "ptr" field needs one more read; using the field address as
// the object reads the entity's own bytes where the component sits. GetComponentPtr above stays
// correct for the INLINE components (CBaseModelEntity::m_Collision is a "ref" field, where the
// field address IS the component).
void* GetComponentPointer(int iHandle, const char* szOwnerClass, const char* szField)
{
	void* pField = GetComponentPtr(iHandle, szOwnerClass, szField);
	if (!pField) return nullptr;
	return Guarded([&] { return *reinterpret_cast<void**>(pField); },
				   [] { return static_cast<void*>(nullptr); });
}

int64_t GetComponentInt(void* pComponent, const char* szClass, const char* szField, int64_t iDefault, int iElement)
{
	if (!pComponent || !__s2sdk_GetEntSchema2) return iDefault;
	return Guarded(
		[&] { return s2sdk::GetEntSchema2(pComponent, plg::string(szClass), plg::string(szField), iElement); },
		[&] { return iDefault; });
}

float GetComponentFloat(void* pComponent, const char* szClass, const char* szField, float flDefault)
{
	if (!pComponent || !__s2sdk_GetEntSchemaFloat2) return flDefault;
	return Guarded(
		[&] { return (float)s2sdk::GetEntSchemaFloat2(pComponent, plg::string(szClass), plg::string(szField), 0); },
		[&] { return flDefault; });
}

void SetComponentInt(void* pComponent, const char* szClass, const char* szField, int64_t iValue, bool bChangeState,
					 int iElement)
{
	if (!pComponent || !__s2sdk_SetEntSchema2) return;
	Guarded(
		[&] {
			s2sdk::SetEntSchema2(pComponent, plg::string(szClass), plg::string(szField), iValue, bChangeState, iElement);
			return 0;
		},
		[] { return 0; });
}

void SetComponentFloat(void* pComponent, const char* szClass, const char* szField, float flValue, bool bChangeState)
{
	if (!pComponent || !__s2sdk_SetEntSchemaFloat2) return;
	Guarded(
		[&] {
			s2sdk::SetEntSchemaFloat2(pComponent, plg::string(szClass), plg::string(szField), flValue, bChangeState, 0);
			return 0;
		},
		[] { return 0; });
}

void SetComponentEntity(void* pComponent, const char* szClass, const char* szField, int iValue, bool bChangeState)
{
	if (!pComponent || !__s2sdk_SetEntSchemaEnt2) return;
	Guarded(
		[&] {
			s2sdk::SetEntSchemaEnt2(pComponent, plg::string(szClass), plg::string(szField), iValue, bChangeState, 0);
			return 0;
		},
		[] { return 0; });
}

// The camera's m_hViewEntity write, without the typed setter's type dereference. SetEntSchemaEnt2
// takes the SchemaKey of s2sdk's per-class schema table and calls schema::IsAtomicType(key.type):
// when the table is built while the engine (re)registers its class info - around map start - the
// entry can hold the invalid sentinel CSchemaType* == -1 and the dereference faults (a SIGSEGV at
// 0x17, `movzbl 0x18(%rdi),%eax` in IsAtomicType with rdi = -1, reached
// from SetEntSchemaEnt2 <- SetComponentEntity("m_hViewEntity") on the first frame after the boot
// changelevel). The raw path needs only the field offset, which is static in the class info (the
// same lookup returned a valid offset in that crash), and the handle width, which is 4 bytes in
// this build (sizeof(CEntityHandle) == sizeof(CHandle<CBaseEntity>) == 4, the size the typed setter
// matched against). The state change is the one the typed setter sent for a networked field: the
// component's chain entity (CPlayerPawnComponent::__m_pChainEntity) carries the value to the pawn.
bool SetComponentEntityRaw(void* pComponent, const char* szClass, const char* szField, int iValue, bool bChangeState)
{
	if (!pComponent || !szClass || !szField) return false;
	if (!__s2sdk_SetEntData2 || !__s2sdk_GetSchemaOffset) return false;

	const int iOffset = Offset(szClass, szField);
	if (iOffset < 0) return false;

	bool bDone = false;
	Guarded(
		[&] {
			s2sdk::SetEntData2(pComponent, iOffset, (int64_t)iValue, (int32_t)sizeof(int32_t), bChangeState,
							   bChangeState ? ChainOffset(szClass) : -1);
			bDone = true;
			return 0;
		},
		[] { return 0; });
	return bDone;
}

// ---------------------------------------------------------------------------------------------

void RawSetInt(int iHandle, int iOffset, int64_t iValue, int iSize, bool bChangeState, int iChainOffset)
{
	if (!IsValidHandle(iHandle) || iOffset < 0 || !__s2sdk_SetEntData) return;
	Guarded(
		[&] {
			s2sdk::SetEntData(iHandle, iOffset, iValue, iSize, bChangeState, iChainOffset);
			return 0;
		},
		[] { return 0; });
}

int64_t RawGetInt(int iHandle, int iOffset, int iSize)
{
	if (!IsValidHandle(iHandle) || iOffset < 0 || !__s2sdk_GetEntData) return 0;
	return Guarded([&] { return s2sdk::GetEntData(iHandle, iOffset, iSize); }, [] { return (int64_t)0; });
}
