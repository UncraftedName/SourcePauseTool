#include "stdafx.h"

#include "..\mesh_renderer.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <algorithm>
#include <iterator>

#include "..\mesh_defs_private.hpp"
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
#define DEBUG_COLOR_MERGED_DYNAMIC_MESH (color32{0, 255, 0, 255})
#define DEBUG_COLOR_CROSS (color32{255, 0, 0, 255})

/*
* We take advantage of two passes the game does: DrawOpaques and DrawTranslucents. Both are called for each view,
* and you may get a different view for each portal, saveglitch overlay, etc. We abstract this away from the user -
* when the user calls DrawMesh() we defer the drawing until those two passes. When rendering opaques, we can
* render the meshes in any order, but the same is not true for translucents. In that case we must figure out our
* own render order. Nothing we do will be perfect - but a good enough solution for many cases is to approximate
* the distance to each mesh from the camera as a single value and sort based on that.
* 
* TODO - try combining opaques & translucents into one mesh before rendering
* 
* This system would not exist without the absurd of times mlugg has helped me while making it; send him lots of
* love, gifts, and fruit baskets.
*/

struct MeshUnitWrapper;

// TODO RENAME TO MESHCOMPONENT?
// TODO make sure that I don't create new meshes while any of these are active since they have pointers to a vector
struct SortedMeshElement
{
	MeshUnitWrapper* unitWrapper;
	MeshVertData* vertData; // null for statics
	IMeshWrapper iMeshWrapper;

	std::weak_ordering operator<=>(const SortedMeshElement& rhs) const;
};

struct MeshRendererInternal
{
	std::vector<MeshUnitWrapper> queuedUnitWrappers;

	struct
	{
		CRendering3dView* rendering3dView;
		CViewSetup* viewSetup;
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

	void FrameCleanup();
	void OnRenderViewPre_Signal(void* thisptr, CViewSetup* cameraView);
	void SetupViewInfo(CRendering3dView* rendering3dView);
	void OnDrawOpaques(CRendering3dView* renderingView);
	void OnDrawTranslucents(CRendering3dView* renderingView);
	void DrawAll(const std::vector<SortedMeshElement>& sortedComponents);

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
	* 3) LoadFeature: Anything that uses the mesh rendering system can check to see if the signal exists.
	*/
	signal.Works = !!ORIG_CRendering3dView__DrawOpaqueRenderables
	               && !!ORIG_CRendering3dView__DrawTranslucentRenderables
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
	const std::shared_ptr<MeshUnit> _staticMeshPtr;
	const DynamicMesh _dynamicToken;
	const RenderCallback callback;

	CallbackInfoOut cbInfoOut;
	MeshPositionInfo posInfo;
	Vector camDistSqrTo;
	float camDistSqr; // translucent sorting metric

	MeshUnitWrapper(DynamicMesh dynamicToken, const RenderCallback& callback = nullptr)
	    : _staticMeshPtr(nullptr)
	    , _dynamicToken(dynamicToken)
	    , callback(callback)
	    , posInfo(g_meshBuilderInternal.GetDynamicMeshFromToken(dynamicToken).posInfo)
	{
	}

	MeshUnitWrapper(const std::shared_ptr<MeshUnit>& staticMeshPtr, const RenderCallback& callback = nullptr)
	    : _staticMeshPtr(staticMeshPtr), _dynamicToken{}, callback(callback), posInfo(staticMeshPtr->posInfo)
	{
	}

	const MeshUnit& GetMeshUnit() const
	{
		return _staticMeshPtr ? *_staticMeshPtr : g_meshBuilderInternal.GetDynamicMeshFromToken(_dynamicToken);
	}

	// returns true if this mesh should be rendered
	bool ApplyCallbackAndCalcCamDist(const CViewSetup& cvs, const cplane_t frustum[6])
	{
		const MeshUnit& meshUnit = GetMeshUnit();

		if (callback)
		{
			// apply callback

			CallbackInfoIn infoIn = {cvs,
			                         meshUnit.posInfo,
			                         spt_meshRenderer.CurrentPortalRenderDepth(),
			                         spt_overlay.renderingOverlay};

			callback(infoIn, cbInfoOut = CallbackInfoOut{});

			if (cbInfoOut.skipRender || cbInfoOut.colorModulate.a == 0)
				return false;

			TransformAABB(cbInfoOut.mat,
			              meshUnit.posInfo.mins,
			              meshUnit.posInfo.maxs,
			              posInfo.mins,
			              posInfo.maxs);
		}
		else
		{
			Assert(!memcmp(&posInfo, &meshUnit.posInfo, sizeof MeshPositionInfo));
		}

		// check if mesh is outside frustum

		for (int i = 0; i < 6; i++)
			if (BoxOnPlaneSide((float*)&posInfo.mins, (float*)&posInfo.maxs, &frustum[i]) == 2)
				return false;

		// calc camera to mesh "distance"

		CalcClosestPointOnAABB(posInfo.mins, posInfo.maxs, cvs.origin, camDistSqrTo);
		if (cvs.origin == camDistSqrTo)
			camDistSqrTo = (posInfo.mins + posInfo.maxs) / 2.f; // if inside cube, use center idfk
		camDistSqr = cvs.origin.DistToSqr(camDistSqrTo);

		return true;
	}

	void Render(const IMeshWrapper& mw)
	{
		if (!mw.iMesh || !mw.material)
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
	debugMeshInfo.descriptionSlices.emplace(debugMeshInfo.sharedDescriptionArray);

	static std::vector<SortedMeshElement> sortedMeshes;
	sortedMeshes.clear();

	// go through each component of each mesh unit and add it to the list of it's eligible for rendering

	for (MeshUnitWrapper& meshUnitWrapper : queuedUnitWrappers)
	{
		if (!meshUnitWrapper.ApplyCallbackAndCalcCamDist(*viewInfo.viewSetup, viewInfo.frustum))
			continue; // the mesh is outside our frustum or the user wants to skip rendering

		if (meshUnitWrapper.callback && meshUnitWrapper.cbInfoOut.colorModulate.a < 1)
			continue; // color modulation makes this mesh unit translucent

		auto shouldRender = [](IMaterial* material)
		{
			return material && !material->GetMaterialVarFlag(MATERIAL_VAR_IGNOREZ)
			       && !material->GetMaterialVarFlag(MATERIAL_VAR_VERTEXALPHA);
		};

		const MeshUnit& meshUnit = meshUnitWrapper.GetMeshUnit();

		if (meshUnit.dynamic)
		{
			for (MeshVertData& vData : meshUnit.dynamicData)
				if (!vData.verts.empty() && shouldRender(vData.material))
					sortedMeshes.emplace_back(&meshUnitWrapper, &vData, IMeshWrapper{});
		}
		else
		{
			for (size_t i = 0; i < meshUnit.staticData.nMeshes; i++)
				if (shouldRender(meshUnit.staticData.meshesArr[i].material))
					sortedMeshes.emplace_back(&meshUnitWrapper,
					                          nullptr,
					                          meshUnit.staticData.meshesArr[i]);
		}
	}

	std::sort(sortedMeshes.begin(), sortedMeshes.end());
	DrawAll(sortedMeshes);
}

void MeshRendererInternal::OnDrawTranslucents(CRendering3dView* renderingView)
{
	VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	SetupViewInfo(renderingView);

	static std::vector<SortedMeshElement> sortedMeshes;
	sortedMeshes.clear();

	for (MeshUnitWrapper& meshUnitWrapper : queuedUnitWrappers)
	{
		if (!meshUnitWrapper.ApplyCallbackAndCalcCamDist(*viewInfo.viewSetup, viewInfo.frustum))
			continue; // the mesh is outside our frustum or the user wants to skip rendering

		if (meshUnitWrapper.callback && meshUnitWrapper.cbInfoOut.colorModulate.a < 1)
			continue; // color modulation makes this mesh unit translucent

		auto shouldRender = [&meshUnitWrapper](IMaterial* material)
		{
			if (!material)
				return false;
			if (meshUnitWrapper.callback && meshUnitWrapper.cbInfoOut.colorModulate.a < 1)
				return true; // a calback made this mesh translucent
			return material->GetMaterialVarFlag(MATERIAL_VAR_IGNOREZ)
			       || material->GetMaterialVarFlag(MATERIAL_VAR_VERTEXALPHA);
		};

		const MeshUnit& meshUnit = meshUnitWrapper.GetMeshUnit();

		if (meshUnit.dynamic)
		{
			for (MeshVertData& vData : meshUnit.dynamicData)
				if (!vData.verts.empty() && shouldRender(vData.material))
					sortedMeshes.emplace_back(&meshUnitWrapper, &vData, IMeshWrapper{});
		}
		else
		{
			for (size_t i = 0; i < meshUnit.staticData.nMeshes; i++)
				if (shouldRender(meshUnit.staticData.meshesArr[i].material))
					sortedMeshes.emplace_back(&meshUnitWrapper,
					                          nullptr,
					                          meshUnit.staticData.meshesArr[i]);
		}
	}

	// TODO debug meshes, combing OnDrawOpaques with OnDrawTranslucents
	std::sort(
	    sortedMeshes.begin(),
	    sortedMeshes.end(),
	    [](const SortedMeshElement& a, const SortedMeshElement& b)
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
		    // TODO check if two meshes overlap from camera's perspective
		    // we already know both objects at least overlap with the frustum, could do something with that
		    if (a.unitWrapper->camDistSqr != b.unitWrapper->camDistSqr)
			    return a.unitWrapper->camDistSqr > b.unitWrapper->camDistSqr;
		    return a < b;
	    });

	DrawAll(sortedMeshes);

	// add crosses showing for all meshes we just rendered

	if (y_spt_draw_mesh_debug.GetBool())
	{
		for (SortedMeshElement& sortedMesh : sortedMeshes)
		{
			auto& posInfo = sortedMesh.unitWrapper->posInfo;
			float maxBoxDim = VectorMaximum(posInfo.maxs - posInfo.mins);
			const float smallest = 1, biggest = 15, falloff = 100;
			// scale point mesh by the AABB size, plot this bad boy in desmos as a function of maxBoxDim
			float size = -falloff * (biggest - smallest) / (maxBoxDim + falloff) + biggest;

			debugMeshInfo.descriptionSlices.top().emplace_back(sortedMesh.unitWrapper->camDistSqrTo,
			                                                   size,
			                                                   DEBUG_COLOR_CROSS);
		}

		// we'll just reuse this array
		sortedMeshes.clear();

		// render all debug meshes (must do here since they have a no z test component) fuck the opaque cross I guess........

		static std::vector<MeshUnitWrapper> debugMeshes;
		debugMeshes.clear();
		debugMeshes.reserve(debugMeshInfo.descriptionSlices.top().size());

		for (auto& debugDesc : debugMeshInfo.descriptionSlices.top())
		{
			debugMeshes.emplace_back(spt_meshBuilder.CreateDynamicMesh(
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
					    mb.AddCross(debugDesc.cross.crossPos,
					                debugDesc.cross.size,
					                debugDesc.color);
				    }
			    }));
		}

		for (MeshUnitWrapper& debugMesh : debugMeshes)
		{
			auto& debugUnit = debugMesh.GetMeshUnit();
			Assert(debugUnit.dynamic);
			for (auto& component : debugUnit.dynamicData)
				if (component.indices.size() > 0)
					sortedMeshes.emplace_back(&debugMesh, &component, IMeshWrapper{});
		}
		std::sort(sortedMeshes.begin(), sortedMeshes.end());
		DrawAll(sortedMeshes);
	}
	debugMeshInfo.descriptionSlices.pop();
}

void MeshRendererInternal::DrawAll(const std::vector<SortedMeshElement>& sortedComponents)
{
	if (sortedComponents.empty())
		return;

	using _it = const SortedMeshElement*;

	_it low;
	_it high = &sortedComponents.front();
	_it end = &sortedComponents.back() + 1;

	while (high != end)
	{
		low = high++;

		if (low->vertData)
		{
			// dynamic - find a suitable range [low,high) that we can batch together
			high = std::find_if(low + 1, end, [low](auto& s) { return (*low <=> s) != 0; });
			// copy the vert data references into this temp buffer
			static std::vector<const MeshVertData*> tmp;
			tmp.clear();
			// TODO try using transform?
			// std::transform(low, high, std::back_insert_iterator(tmp), [](const SortedMeshElement& s) { return s.vertData; });
			for (auto it = low; it < high; it++)
				tmp.push_back(it->vertData);

			// batch the whole range
			g_meshBuilderInternal.BeginDynamicMeshCreation(&tmp.front(), &tmp.back() + 1, true);
			IMeshWrapper mw;
			while (mw = g_meshBuilderInternal.GetNextIMeshWrapper(), mw.iMesh)
			{
				low->unitWrapper->Render(mw);

				if (y_spt_draw_mesh_debug.GetBool())
				{
					auto& lastSlice = debugMeshInfo.descriptionSlices.top();
					Vector batchedMins{INFINITY};
					Vector batchedMaxs{-INFINITY};
					for (auto it = low; it < high; it++)
					{
						auto& posInfo = it->unitWrapper->posInfo;
						VectorMin(posInfo.mins, batchedMins, batchedMins);
						VectorMax(posInfo.maxs, batchedMaxs, batchedMaxs);

						lastSlice.emplace_back(posInfo.mins - Vector{1},
						                       posInfo.maxs + Vector{1},
						                       it->unitWrapper->callback
						                           ? DEBUG_COLOR_DYNAMIC_MESH_WITH_CALLBACK
						                           : DEBUG_COLOR_DYNAMIC_MESH);
					}
					if (std::distance(low, high) > 1 && y_spt_draw_mesh_debug.GetInt() >= 2)
					{
						lastSlice.emplace_back(batchedMins - Vector{2},
						                       batchedMaxs + Vector{2},
						                       DEBUG_COLOR_MERGED_DYNAMIC_MESH);
					}
				}
			}
		}
		else
		{
			// a single static mesh
			low->unitWrapper->Render(low->iMeshWrapper);
			if (y_spt_draw_mesh_debug.GetBool())
			{
				auto& lastSlice = debugMeshInfo.descriptionSlices.top();
				lastSlice.emplace_back(low->unitWrapper->posInfo.mins - Vector{1},
				                       low->unitWrapper->posInfo.maxs + Vector{1},
				                       DEBUG_COLOR_STATIC_MESH);
			}
		}
	}
}

/**************************************** SPACESHIP ****************************************/

std::weak_ordering SortedMeshElement::operator<=>(const SortedMeshElement& rhs) const
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

/**************************************** MESH RENDERER ****************************************/

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
	const MeshUnit& meshUnit = g_meshBuilderInternal.GetDynamicMeshFromToken(dynamicMesh);
	if (!meshUnit.dynamicData.empty())
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
	if (staticMesh.meshPtr->staticData.nMeshes > 0)
		g_meshRendererInternal.queuedUnitWrappers.emplace_back(staticMesh.meshPtr, callback);
}

#endif
