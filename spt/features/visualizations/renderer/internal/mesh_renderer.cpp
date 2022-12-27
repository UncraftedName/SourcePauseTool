#include "stdafx.h"

#include "..\mesh_renderer.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <algorithm>
#include <iterator>

#include "internal_defs.hpp"
#include "interfaces.hpp"
#include "signals.hpp"

ConVar y_spt_draw_mesh_debug("y_spt_draw_mesh_debug",
                             "0",
                             FCVAR_CHEAT | FCVAR_DONTRECORD,
                             "Draws the distance metric and AABBs of all meshes.");

// TODO comments

#define DEBUG_COLOR_STATIC_MESH (color32{150, 20, 10, 255})
#define DEBUG_COLOR_DYNAMIC_MESH (color32{0, 0, 255, 255})
#define DEBUG_COLOR_DYNAMIC_MESH_WITH_CALLBACK (color32{255, 150, 50, 255})
#define DEBUG_COLOR_MERGED_DYNAMIC_MESH_OPAQUE (color32{0, 255, 0, 255})
#define DEBUG_COLOR_MERGED_DYNAMIC_MESH_TRANSLUCENT (color32{150, 255, 200, 255})
#define DEBUG_COLOR_CROSS (color32{255, 0, 0, 255})

/*
* We take advantage of two passes the game does: DrawOpaques and DrawTranslucents. Both are called for each view,
* and you may get a different view for each portal, saveglitch overlay, etc. We abstract this away from the user -
* when the user calls DrawMesh() we defer the drawing until those two passes. When rendering opaques, we can
* render the meshes in any order, but the same is not true for translucents. In that case we must figure out our
* own render order. Nothing we do will be perfect - but a good enough solution for many cases is to approximate
* the distance to each mesh from the camera as a single value and sort based on that.
* 
* This system would not exist without the absurd of times mlugg has helped me while making it; send him lots of
* love, gifts, and fruit baskets.
*/

struct MeshUnitWrapper;

struct MeshRendererInternal
{
	std::vector<MeshUnitWrapper> queuedUnitWrappers;

	struct
	{
		const CRendering3dView* rendering3dView;
		const CViewSetup* viewSetup;
		cplane_t frustum[FRUSTUM_NUMPLANES];
	} viewInfo;

	struct
	{
		struct DebugMeshDesc
		{
			union
			{
				struct
				{
					Vector mins, maxs;
				} box;

				struct
				{
					Vector crossPos;
					float size;
				} cross;
			};

			bool isBox;
			color32 color;

			DebugMeshDesc(){};

			DebugMeshDesc(const Vector& mins, const Vector& maxs, color32 c)
			    : box{mins, maxs}, isBox(true), color(c)
			{
			}

			DebugMeshDesc(const Vector& crossPos, float size, color32 c)
			    : cross{crossPos, size}, isBox(false), color(c)
			{
			}
		};

		std::vector<DebugMeshDesc> sharedDescriptionArray;
		VectorStack<VectorSlice<DebugMeshDesc>> descriptionSlices;

	} debugMeshInfo;

	using DebugDescList = decltype(debugMeshInfo.descriptionSlices)::value_type;

	void OnRenderViewPre_Signal(void* thisptr, CViewSetup* cameraView);
	void FrameCleanup();
	void OnDrawOpaques(CRendering3dView* rendering3dView);
	void OnDrawTranslucents(CRendering3dView* rendering3dView);

	void SetupViewInfo(CRendering3dView* rendering3dView);
	ComponentRange CollectRenderableComponents(bool opaques);
	void AddDebugCrosses(ConstComponentRange range, bool opaques);
	void AddDebugBox(ConstComponentRange range, bool opaques);
	void DrawDebugMeshes();
	void DrawAll(ConstComponentRange range, bool addDebugMeshes, bool opaques);

	void AddDebugCrosses(DebugDescList& debugList, ConstComponentRange range, bool opaques);
	void AddDebugBox(DebugDescList& debugList, ConstComponentRange range, bool opaques);
	void DrawDebugMeshes(DebugDescList& debugList);

} g_meshRendererInternal;

/**************************************** MESH RENDERER FEATURE ****************************************/

struct CPortalRender
{
#ifdef SSDK2007
	byte _pad[0x57c];
#else
	byte _pad[0x918];
#endif
	int m_iViewRecursionLevel; // you could probably infer this from the call depth of DrawTranslucents
};

namespace patterns
{
	PATTERNS(CRendering3dView__DrawOpaqueRenderables,
	         "5135",
	         "55 8D 6C 24 8C 81 EC 94 00 00 00",
	         "7462488",
	         "55 8B EC 81 EC 80 00 00 00 8B 15 ?? ?? ?? ??");
	PATTERNS(CRendering3dView__DrawTranslucentRenderables,
	         "5135",
	         "55 8B EC 83 EC 34 53 8B D9 8B 83 94 00 00 00 8B 13 56 8D B3 94 00 00 00",
	         "5135-hl2",
	         "55 8B EC 83 EC 34 83 3D ?? ?? ?? ?? 00",
	         "1910503",
	         "55 8B EC 81 EC 9C 00 00 00 53 56 8B F1 8B 86 E8 00 00 00 8B 16 57 8D BE E8 00 00 00",
	         "7462488",
	         "55 8B EC 81 EC A0 00 00 00 53 8B D9",
	         "7467727-hl2",
	         "55 8B EC 81 EC A0 00 00 00 83 3D ?? ?? ?? ?? 00");
	// this is the static OnRenderStart(), not the virtual one; only used for portal render depth
	PATTERNS(OnRenderStart,
	         "5135",
	         "56 8B 35 ?? ?? ?? ?? 8B 06 8B 50 64 8B CE FF D2 8B 0D ?? ?? ?? ??",
	         "7462488",
	         "55 8B EC 83 EC 10 53 56 8B 35 ?? ?? ?? ?? 8B CE 57 8B 06 FF 50 64 8B 0D ?? ?? ?? ??");
} // namespace patterns

bool MeshRendererFeature::ShouldLoadFeature()
{
	if (!interfaces::materialSystem)
	{
		DevWarning("Mesh rendering not available because materialSystem was not initialized!\n");
		return false;
	}
	return true;
}

void MeshRendererFeature::InitHooks()
{
	HOOK_FUNCTION(client, CRendering3dView__DrawOpaqueRenderables);
	HOOK_FUNCTION(client, CRendering3dView__DrawTranslucentRenderables);
	FIND_PATTERN(client, OnRenderStart);
}

void MeshRendererFeature::PreHook()
{
	/*
	* 1) InitHooks: spt_overlay finds the render function.
	* 2) PreHook: spt_overlay may connect the RenderViewSignal. To not depend on feature load order
	*    we cannot check if the signal exists, but we can check if the RenderView function was found.
	* 3) LoadFeature: Anything that uses the mesh rendering system can check to see if the signal works.
	*/
	signal.Works = ORIG_CRendering3dView__DrawOpaqueRenderables && ORIG_CRendering3dView__DrawTranslucentRenderables
	               && spt_overlay.ORIG_CViewRender__RenderView;

	if (ORIG_OnRenderStart)
	{
		int idx = GetPatternIndex((void**)&ORIG_OnRenderStart);
		uint32_t offs[] = {18, 24};
		if (idx >= 0 && idx < sizeof(offs) / sizeof(uint32_t))
			g_pPortalRender = *(CPortalRender***)((uintptr_t)ORIG_OnRenderStart + offs[idx]);
	}

	g_meshMaterialMgr.Load();
}

void MeshRendererFeature::LoadFeature()
{
	if (!signal.Works)
		return;
	RenderViewPre_Signal.Connect(&g_meshRendererInternal, &MeshRendererInternal::OnRenderViewPre_Signal);
	InitConcommandBase(y_spt_draw_mesh_debug);
}

void MeshRendererFeature::UnloadFeature()
{
	signal.Clear();
	g_meshRendererInternal.FrameCleanup();
	g_meshMaterialMgr.Unload();
	StaticMesh::DestroyAll();
	g_pPortalRender = nullptr;
}

int MeshRendererFeature::CurrentPortalRenderDepth() const
{
	if (!g_pPortalRender || !*g_pPortalRender)
		return -1;
	return (**g_pPortalRender).m_iViewRecursionLevel;
}

HOOK_THISCALL(void, MeshRendererFeature, CRendering3dView__DrawOpaqueRenderables, int param)
{
	// HACK - param is a bool in SSDK2007 and enum in SSDK2013
	// render order shouldn't matter here
	spt_meshRenderer.ORIG_CRendering3dView__DrawOpaqueRenderables(thisptr, 0, param);
	if (spt_meshRenderer.signal.Works)
		g_meshRendererInternal.OnDrawOpaques((CRendering3dView*)thisptr);
}

HOOK_THISCALL(void, MeshRendererFeature, CRendering3dView__DrawTranslucentRenderables, bool inSkybox, bool shadowDepth)
{
	// render order matters here, render our stuff on top of other translucents
	spt_meshRenderer.ORIG_CRendering3dView__DrawTranslucentRenderables(thisptr, 0, inSkybox, shadowDepth);
	if (spt_meshRenderer.signal.Works)
		g_meshRendererInternal.OnDrawTranslucents((CRendering3dView*)thisptr);
}

/**************************************** MESH UNIT WRAPPER ****************************************/

struct MeshUnitWrapper
{
	// keep statics alive as long as we render
	const std::shared_ptr<StaticMeshUnit> _staticMeshPtr;
	const DynamicMeshToken _dynamicToken;
	const RenderCallback callback;

	CallbackInfoOut cbInfoOut;
	MeshPositionInfo posInfo;
	Vector camDistSqrTo;
	float camDistSqr; // translucent sorting metric

	MeshUnitWrapper(DynamicMeshToken dynamicToken, const RenderCallback& callback = nullptr)
	    : _staticMeshPtr(nullptr)
	    , _dynamicToken(dynamicToken)
	    , callback(callback)
	    , posInfo(g_meshBuilderInternal.GetDynamicMeshFromToken(dynamicToken).posInfo)
	{
	}

	MeshUnitWrapper(const std::shared_ptr<StaticMeshUnit>& staticMeshPtr, const RenderCallback& callback = nullptr)
	    : _staticMeshPtr(staticMeshPtr), _dynamicToken{}, callback(callback), posInfo(staticMeshPtr->posInfo)
	{
	}

	// returns true if this mesh should be rendered
	bool ApplyCallbackAndCalcCamDist()
	{
		auto& viewInfo = g_meshRendererInternal.viewInfo;

		if (callback)
		{
			// apply callback

			const MeshPositionInfo& unitPosInfo =
			    _staticMeshPtr ? _staticMeshPtr->posInfo
			                   : g_meshBuilderInternal.GetDynamicMeshFromToken(_dynamicToken).posInfo;

			CallbackInfoIn infoIn = {
			    *viewInfo.viewSetup,
			    unitPosInfo,
			    spt_meshRenderer.CurrentPortalRenderDepth(),
			    spt_overlay.renderingOverlay,
			};

			callback(infoIn, cbInfoOut = CallbackInfoOut{});

			if (cbInfoOut.skipRender || cbInfoOut.colorModulate.a == 0)
				return false;
			TransformAABB(cbInfoOut.mat, unitPosInfo.mins, unitPosInfo.maxs, posInfo.mins, posInfo.maxs);
		}

		// do frustum check

		auto& frustum = g_meshRendererInternal.viewInfo.frustum;
		for (int i = 0; i < 6; i++)
			if (BoxOnPlaneSide((float*)&posInfo.mins, (float*)&posInfo.maxs, &frustum[i]) == 2)
				return false;

		// calc camera to mesh "distance"

		CalcClosestPointOnAABB(posInfo.mins, posInfo.maxs, viewInfo.viewSetup->origin, camDistSqrTo);
		if (viewInfo.viewSetup->origin == camDistSqrTo)
			camDistSqrTo = (posInfo.mins + posInfo.maxs) / 2.f; // if inside cube, use center idfk
		camDistSqr = viewInfo.viewSetup->origin.DistToSqr(camDistSqrTo);

		return true;
	}

	void Render(const IMeshWrapper mw)
	{
		if (!mw.iMesh)
			return;

		CMatRenderContextPtr context{interfaces::materialSystem};
		if (callback)
		{
			color32 cMod = cbInfoOut.colorModulate;
			mw.material->ColorModulate(cMod.r / 255.f, cMod.g / 255.f, cMod.b / 255.f);
			mw.material->AlphaModulate(cMod.a / 255.f);
			context->MatrixMode(MATERIAL_MODEL);
			context->PushMatrix();
			context->LoadMatrix(cbInfoOut.mat);
		}
		else
		{
			mw.material->ColorModulate(1, 1, 1);
			mw.material->AlphaModulate(1);
		}
		context->Bind(mw.material);
		mw.iMesh->Draw();
		if (!_staticMeshPtr)
			context->Flush();
		if (callback)
			context->PopMatrix();
	}
};

/**************************************** MESH RENDER FEATURE ****************************************/

void MeshRendererInternal::FrameCleanup()
{
	queuedUnitWrappers.clear();
	g_meshBuilderInternal.FrameCleanup();
	Assert(debugMeshInfo.descriptionSlices.empty());
}

void MeshRendererInternal::OnRenderViewPre_Signal(void* thisptr, CViewSetup* cameraView)
{
	// ensure we only run once per frame
	if (spt_overlay.renderingOverlay || !spt_meshRenderer.signal.Works)
		return;
	VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	FrameCleanup();
	g_meshRenderFrameNum++;
	g_inMeshRenderSignal = true;
	MeshRendererDelegate renderDelgate{};
	spt_meshRenderer.signal(renderDelgate);
	g_inMeshRenderSignal = false;
}

void MeshRendererInternal::SetupViewInfo(CRendering3dView* rendering3dView)
{
	viewInfo.rendering3dView = rendering3dView;

	// if only more stuff was public ://

	viewInfo.viewSetup = (CViewSetup*)((uintptr_t)rendering3dView + 8);

	// these are inward facing planes, convert from VPlane to cplane_t for fast frustum test
	for (int i = 0; i < FRUSTUM_NUMPLANES; i++)
	{
		VPlane* vp = *(VPlane**)(viewInfo.viewSetup + 1) + i;
		viewInfo.frustum[i] = {.normal = vp->m_Normal, .dist = vp->m_Dist, .type = 255};
		viewInfo.frustum[i].signbits = SignbitsForPlane(&viewInfo.frustum[i]);
	}
}

void MeshRendererInternal::OnDrawOpaques(CRendering3dView* renderingView)
{
	VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	SetupViewInfo(renderingView);

	// push a new debug slice, the corresponding pop will be in DrawTranslucents
	debugMeshInfo.descriptionSlices.emplace(debugMeshInfo.sharedDescriptionArray);

	ComponentRange range = CollectRenderableComponents(true);
	std::sort(range.first, range.second);
	DrawAll(range, y_spt_draw_mesh_debug.GetBool(), true);
}

void MeshRendererInternal::OnDrawTranslucents(CRendering3dView* renderingView)
{
	VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	SetupViewInfo(renderingView);

	ComponentRange range = CollectRenderableComponents(false);

	std::sort(range.first,
	          range.second,
	          [](const MeshComponent& a, const MeshComponent& b)
	          {
		          IMaterial* matA = a.vertData ? a.vertData->material : a.iMeshWrapper.material;
		          IMaterial* matB = b.vertData ? b.vertData->material : b.iMeshWrapper.material;
		          if (matA != matB)
		          {
			          bool ignoreZA = matA->GetMaterialVarFlag(MATERIAL_VAR_IGNOREZ);
			          bool ignoreZB = matB->GetMaterialVarFlag(MATERIAL_VAR_IGNOREZ);
			          if (ignoreZA != ignoreZB)
				          return ignoreZB; // TODO test me!!
		          }
		          if (a.unitWrapper->camDistSqr != b.unitWrapper->camDistSqr)
			          return a.unitWrapper->camDistSqr > b.unitWrapper->camDistSqr;
		          return a < b;
	          });

	DrawAll(range, y_spt_draw_mesh_debug.GetBool(), false);

	if (y_spt_draw_mesh_debug.GetBool())
	{
		AddDebugCrosses(debugMeshInfo.descriptionSlices.top(), range, false);
		DrawDebugMeshes(debugMeshInfo.descriptionSlices.top());
	}
	debugMeshInfo.descriptionSlices.pop();
}

ComponentRange MeshRendererInternal::CollectRenderableComponents(bool opaques)
{
	static std::vector<MeshComponent> components;
	components.clear();

	for (MeshUnitWrapper& unitWrapper : queuedUnitWrappers)
	{
		if (!unitWrapper.ApplyCallbackAndCalcCamDist())
			continue; // the mesh is outside our frustum or the user wants to skip rendering

		if (unitWrapper.callback && unitWrapper.cbInfoOut.colorModulate.a < 1)
			continue; // color modulation makes this mesh unit translucent

		auto shouldRender = [&unitWrapper, opaques](IMaterial* material)
		{
			if (!material)
				return false;

			if (unitWrapper.callback)
				if (unitWrapper.cbInfoOut.colorModulate.a < 255)
					return !opaques; // callback changed alpha component, make translucent if < 1 otherwise opaque

			bool opaqueMaterial = !material->GetMaterialVarFlag(MATERIAL_VAR_IGNOREZ)
			                      && !material->GetMaterialVarFlag(MATERIAL_VAR_VERTEXALPHA);

			return opaques == opaqueMaterial;
		};

		if (unitWrapper._staticMeshPtr)
		{
			auto& unit = *unitWrapper._staticMeshPtr;
			for (size_t i = 0; i < unit.nMeshes; i++)
				if (shouldRender(unit.meshesArr[i].material))
					components.emplace_back(&unitWrapper, nullptr, unit.meshesArr[i]);
		}
		else
		{
			auto& unit = g_meshBuilderInternal.GetDynamicMeshFromToken(unitWrapper._dynamicToken);
			for (MeshVertData& vData : unit.vDataSlice)
				if (!vData.verts.empty() && shouldRender(vData.material))
					components.emplace_back(&unitWrapper, &vData, IMeshWrapper{});
		}
	}
	return ComponentRange{components.begin(), components.end()};
}

void MeshRendererInternal::AddDebugCrosses(DebugDescList& debugList, ConstComponentRange range, bool opaques)
{
	(void)opaques;
	for (auto it = range.first; it < range.second; it++)
	{
		const MeshPositionInfo& posInfo = it->unitWrapper->posInfo;
		float maxDiameter = VectorMaximum(posInfo.maxs - posInfo.mins);
		const float smallest = 1, biggest = 15, falloff = 100;
		// scale cross by the AABB size, plot this bad boy in desmos as a function of maxBoxDim
		float size = -falloff * (biggest - smallest) / (maxDiameter + falloff) + biggest;

		debugList.emplace_back(it->unitWrapper->camDistSqrTo, size, DEBUG_COLOR_CROSS);
	}
}

void MeshRendererInternal::DrawDebugMeshes(DebugDescList& debugList)
{
	static std::vector<MeshUnitWrapper> debugUnitWrappers;
	debugUnitWrappers.clear();

	for (auto& debugDesc : debugList)
	{
		debugUnitWrappers.emplace_back(spt_meshBuilder.CreateDynamicMesh(
		    [&](MeshBuilderDelegate& mb)
		    {
			    if (debugDesc.isBox)
			    {
				    mb.AddBox(vec3_origin,
				              debugDesc.box.mins,
				              debugDesc.box.maxs,
				              vec3_angle,
				              MeshColor::Wire(debugDesc.color),
				              false);
			    }
			    else
			    {
				    mb.AddCross(debugDesc.cross.crossPos, debugDesc.cross.size, debugDesc.color);
			    }
		    }));
	}

	static std::vector<MeshComponent> debugComponents;
	debugComponents.clear();

	for (MeshUnitWrapper& debugMesh : debugUnitWrappers)
	{
		Assert(!debugMesh._staticMeshPtr);

		auto& debugUnit = g_meshBuilderInternal.GetDynamicMeshFromToken(debugMesh._dynamicToken);

		for (auto& component : debugUnit.vDataSlice)
			if (component.indices.size() > 0)
				debugComponents.emplace_back(&debugMesh, &component, IMeshWrapper{});
	}
	std::sort(debugComponents.begin(), debugComponents.end());
	DrawAll({debugComponents.begin(), debugComponents.end()}, false, true);
}

void MeshRendererInternal::DrawAll(ConstComponentRange fullRange, bool addDebugMeshes, bool opaques)
{
	if (std::distance(fullRange.first, fullRange.second) == 0)
		return;

	ConstComponentRange range{{}, fullRange.first};

	while (range.second != fullRange.second)
	{
		range.first = range.second++;

		if (range.first->vertData)
		{
			range.second = std::find_if(range.first + 1,
			                            fullRange.second,
			                            [=](auto& mc) { return (*range.first <=> mc) != 0; });

			// batch the whole range
			g_meshBuilderInternal.BeginIMeshCreation(range, true);
			IMeshWrapper mw;
			while (mw = g_meshBuilderInternal.GetNextIMeshWrapper(), mw.iMesh)
			{
				range.first->unitWrapper->Render(mw);
				if (addDebugMeshes)
				{
					AddDebugBox(debugMeshInfo.descriptionSlices.top(),
					            g_meshBuilderInternal.creationStatus.batchRange,
					            opaques);
				}
			}
		}
		else
		{
			// a single static mesh
			range.first->unitWrapper->Render(range.first->iMeshWrapper);
			if (addDebugMeshes)
				AddDebugBox(debugMeshInfo.descriptionSlices.top(), range, opaques);
		}
	}
}

void MeshRendererInternal::AddDebugBox(DebugDescList& debugList, ConstComponentRange range, bool opaques)
{
	if (range.first->vertData)
	{
		Vector batchedMins{INFINITY};
		Vector batchedMaxs{-INFINITY};
		for (auto it = range.first; it < range.second; it++)
		{
			VectorMin(it->unitWrapper->posInfo.mins, batchedMins, batchedMins);
			VectorMax(it->unitWrapper->posInfo.maxs, batchedMaxs, batchedMaxs);

			debugList.emplace_back(it->unitWrapper->posInfo.mins - Vector{1.f},
			                       it->unitWrapper->posInfo.maxs + Vector{1.f},
			                       it->unitWrapper->callback ? DEBUG_COLOR_DYNAMIC_MESH_WITH_CALLBACK
			                                                 : DEBUG_COLOR_DYNAMIC_MESH);
		}
		if (std::distance(range.first, range.second) > 1 && y_spt_draw_mesh_debug.GetInt() >= 2)
		{
			debugList.emplace_back(batchedMins - Vector{opaques ? 2.5f : 2.f},
			                       batchedMaxs + Vector{opaques ? 2.5f : 2.f},
			                       opaques ? DEBUG_COLOR_MERGED_DYNAMIC_MESH_OPAQUE
			                               : DEBUG_COLOR_MERGED_DYNAMIC_MESH_TRANSLUCENT);
		}
	}
	else
	{
		Assert(std::distance(range.first, range.second) == 1);
		debugList.emplace_back(range.first->unitWrapper->posInfo.mins - Vector{1},
		                       range.first->unitWrapper->posInfo.maxs + Vector{1},
		                       DEBUG_COLOR_STATIC_MESH);
	}
}

/**************************************** SPACESHIP ****************************************/

std::weak_ordering MeshComponent::operator<=>(const MeshComponent& rhs) const
{
	using W = std::weak_ordering;
	// group dynamics together
	if (!vertData != !rhs.vertData)
		return !vertData <=> !rhs.vertData;
	// between dynamics
	if (vertData && rhs.vertData)
	{
		// group the same primitive type together
		if (vertData->type != rhs.vertData->type)
			return vertData->type <=> rhs.vertData->type;
		// group those without a callback together
		if (!unitWrapper->callback != !rhs.unitWrapper->callback)
			return !unitWrapper->callback <=> !rhs.unitWrapper->callback;
		// if both have a callback, only group together if it's the same unit (the callback will be the same)
		if (unitWrapper->callback && rhs.unitWrapper->callback)
			return unitWrapper <=> rhs.unitWrapper;
	}
	else
	{
		// between statics, if both are in the same unit they'll have the same material, callback, colormod, etc.
		if (&unitWrapper == &rhs.unitWrapper)
			return W::equivalent;
	}
	// group the same materials together
	IMaterial* matA = vertData ? vertData->material : iMeshWrapper.material;
	IMaterial* matB = rhs.vertData ? rhs.vertData->material : rhs.iMeshWrapper.material;
	if (matA != matB)
		return matA <=> matB;
	// group the same color mod for a material together, switching color mod seems to be very slow
	if (unitWrapper->callback && rhs.unitWrapper->callback)
	{
		return *reinterpret_cast<int*>(&unitWrapper->cbInfoOut.colorModulate)
		       <=> *reinterpret_cast<int*>(&rhs.unitWrapper->cbInfoOut.colorModulate);
	}
	return W::equivalent;
}

/**************************************** MESH RENDERER DELEGATE ****************************************/

void MeshRendererDelegate::DrawMesh(const DynamicMesh& dynamicMesh, const RenderCallback& callback)
{
	if (!g_inMeshRenderSignal)
	{
		AssertMsg(0, "spt: Meshes can only be drawn in MeshRenderSignal!");
		return;
	}
	if (dynamicMesh.createdFrame != g_meshRenderFrameNum)
	{
		AssertMsg(0, "spt: Attempted to reuse a dynamic mesh between frames");
		Warning("spt: Can only draw dynamic meshes on the frame they were created!\n");
		return;
	}
	const DynamicMeshUnit& meshUnit = g_meshBuilderInternal.GetDynamicMeshFromToken(dynamicMesh);
	if (!meshUnit.vDataSlice.empty())
		g_meshRendererInternal.queuedUnitWrappers.emplace_back(dynamicMesh, callback);
}

void MeshRendererDelegate::DrawMesh(const StaticMesh& staticMesh, const RenderCallback& callback)
{
	if (!g_inMeshRenderSignal)
	{
		AssertMsg(0, "spt: Meshes can only be drawn in MeshRenderSignal!");
		return;
	}
	if (!staticMesh.Valid())
	{
		AssertMsg(0, "spt: This static mesh has been destroyed!");
		Warning("spt: Attempting to draw an invalid static mesh!\n");
		return;
	}
	if (staticMesh.meshPtr->nMeshes > 0)
		g_meshRendererInternal.queuedUnitWrappers.emplace_back(staticMesh.meshPtr, callback);
}

#endif
