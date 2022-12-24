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

// can't put this as a member in the feature without including defs_private in the header, so we make it a global
static std::vector<struct MeshUnitWrapper> g_meshUnitWrappers;

/**************************************** MESH RENDERER FEATURE ****************************************/

struct CPortalRender
{
#ifdef SSDK2007
	byte _pad[0x57c];
#else
	byte _pad[0x918];
#endif
	int m_iViewRecursionLevel;
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
	signal.Works = Works();

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
	if (!Works())
		return;
	RenderViewPre_Signal.Connect(this, &MeshRendererFeature::OnRenderViewPre_Signal);
	InitConcommandBase(y_spt_draw_mesh_debug);
}

void MeshRendererFeature::UnloadFeature()
{
	signal.Clear();
	FrameCleanup();
	g_meshMaterialMgr.Unload();
	StaticMesh::DestroyAll();
	g_pPortalRender = nullptr;
}

bool MeshRendererFeature::Works() const
{
	/*
	* 1) InitHooks: spt_overlay finds the render function.
	* 2) PreHook: spt_overlay may connect the RenderViewSignal. To not depend on feature load order
	*    we cannot check if the signal exists, but we can check if the RenderView function was found.
	* 3) LoadFeature: Anything that uses the mesh rendering system can check to see if the signal exists.
	*/
	return !!ORIG_CRendering3dView__DrawOpaqueRenderables && !!ORIG_CRendering3dView__DrawTranslucentRenderables
	       && spt_overlay.ORIG_CViewRender__RenderView;
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
	if (spt_meshRenderer.Works())
		spt_meshRenderer.OnDrawOpaques((CRendering3dView*)thisptr);
}

HOOK_THISCALL(void, MeshRendererFeature, CRendering3dView__DrawTranslucentRenderables, bool inSkybox, bool shadowDepth)
{
	// render order matters here, render our stuff on top of other translucents
	spt_meshRenderer.ORIG_CRendering3dView__DrawTranslucentRenderables(thisptr, 0, inSkybox, shadowDepth);
	if (spt_meshRenderer.Works())
		spt_meshRenderer.OnDrawTranslucents((CRendering3dView*)thisptr);
}

/**************************************** MESH UNIT WRAPPER ****************************************/

struct MeshUnitWrapper
{
	// keep statics alive as long as we render
	const std::shared_ptr<MeshUnit> _staticMeshPtr;
	const DynamicMesh _dynamicToken;
	const RenderCallback callback;

	CallbackInfoOut cbInfoOut;
	MeshPositionInfo newPosInfo;
	Vector camDistSqrTo;
	float camDistSqr; // translucent sorting metric

	MeshUnitWrapper(DynamicMesh dynamicToken, const RenderCallback& callback)
	    : _staticMeshPtr(nullptr), _dynamicToken(dynamicToken), callback(callback)
	{
	}

	MeshUnitWrapper(const std::shared_ptr<MeshUnit>& staticMeshPtr, const RenderCallback& callback)
	    : _staticMeshPtr(staticMeshPtr), _dynamicToken{}, callback(callback)
	{
	}

	const MeshUnit& GetMeshUnit() const
	{
		return _staticMeshPtr ? *_staticMeshPtr : g_meshBuilderInternal.GetDynamicMeshFromToken(_dynamicToken);
	}

	// returns true if this mesh should be rendered
	bool ApplyCallbackAndCalcCamDist(const CViewSetup& cvs, const VPlane frustum[6])
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

			if (cbInfoOut.skipRender)
				return false;

			TransformAABB(cbInfoOut.mat,
			              meshUnit.posInfo.mins,
			              meshUnit.posInfo.maxs,
			              newPosInfo.mins,
			              newPosInfo.maxs);
		}

		const MeshPositionInfo& posInfo = callback ? newPosInfo : meshUnit.posInfo;

		// check if mesh is outside frustum

		// TODO convert to mathlib BoxOnPlaneSide
		for (int i = 0; i < 6; i++)
			if (frustum[i].BoxOnPlaneSide(posInfo.mins, posInfo.maxs) == SIDE_BACK)
				return false;

		// calc camera to mesh "distance"

		CalcClosestPointOnAABB(posInfo.mins, posInfo.maxs, cvs.origin, camDistSqrTo);
		if (cvs.origin == camDistSqrTo)
			camDistSqrTo = (posInfo.mins + posInfo.maxs) / 2.f; // if inside cube, use center idfk
		camDistSqr = cvs.origin.DistToSqr(camDistSqrTo);

		return true;
	}

	/*void RenderDebugMesh()
	{
		static StaticMesh debugBoxMesh, debugCrossMesh;

		if (!debugBoxMesh.Valid() || !debugCrossMesh.Valid())
		{

			// make sure both meshes have just one component

			debugBoxMesh = MB_STATIC(
			    {
				    const Vector v0 = vec3_origin;
				    mb.AddBox(v0, v0, Vector(1), vec3_angle, MeshColor::Wire({0, 255, 100, 255}));
			    },
			    ZTEST_NONE);

			debugCrossMesh = MB_STATIC(mb.AddCross({0, 0, 0}, 2, {255, 0, 0, 255}););
		}

		MeshUnit* boxMeshUnit = debugBoxMesh.meshPtr.get();
		MeshUnit* crossMeshUnit = debugCrossMesh.meshPtr.get();

		CMatRenderContextPtr context{interfaces::materialSystem};

		IMaterial* debugMaterial = g_meshMaterialMgr.matAlphaNoZ;
		if (!debugMaterial)
			return;

		// figure out box mesh dims
		const MeshUnit& myWrapper = GetMeshUnit();
		Assert(!myWrapper.faceComponent.Empty() || !myWrapper.lineComponent.Empty());
		auto& myPosInfo = callback ? newPosInfo : myWrapper.posInfo;
		const Vector nudge(0.05);
		Vector size = (myPosInfo.maxs + nudge) - (myPosInfo.mins - nudge);
		matrix3x4_t mat({size.x, 0, 0}, {0, size.y, 0}, {0, 0, size.z}, myPosInfo.mins - nudge);

		context->MatrixMode(MATERIAL_MODEL);
		context->PushMatrix();
		context->LoadMatrix(mat);
		context->Bind(debugMaterial);
		IMesh* iMesh = boxMeshUnit->meshes[0].iMesh;
		if (iMesh)
			iMesh->Draw();

		const float smallest = 1, biggest = 15, falloff = 100;
		float maxBoxDim = VectorMaximum(size);
		// scale point mesh by the AABB size, plot this bad boy in desmos as a function of maxBoxDim
		float pointSize = -falloff * (biggest - smallest) / (maxBoxDim + falloff) + biggest;
		mat.Init({pointSize, 0, 0}, {0, pointSize, 0}, {0, 0, pointSize}, camDistSqrTo);

		IMaterial* crossMaterial = g_meshMaterialMgr.GetMaterial(crossMeshUnit->lineComponent.opaque,
		                                                         crossMeshUnit->lineComponent.zTest,
		                                                         {255, 255, 255, 255});
		Assert(crossMaterial);

		context->LoadMatrix(mat);
		context->Bind(crossMaterial);
		iMesh = crossMeshUnit->lineComponent.GetIMesh(*crossMeshUnit, crossMaterial);
		if (iMesh)
			iMesh->Draw();
		context->PopMatrix();
	}*/

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

void MeshRendererFeature::FrameCleanup()
{
	g_meshUnitWrappers.clear();
	g_meshBuilderInternal.FrameCleanup();
}

void MeshRendererFeature::OnRenderViewPre_Signal(void* thisptr, CViewSetup* cameraView)
{
	// ensure we only run once per frame
	if (spt_overlay.renderingOverlay || !signal.Works)
		return;
	VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	FrameCleanup();
	g_meshRenderFrameNum++;
	g_inMeshRenderSignal = true;
	MeshRendererDelegate renderDelgate{};
	signal(renderDelgate);
	g_inMeshRenderSignal = false;
}

void MeshRendererFeature::SetupViewInfo(CRendering3dView* rendering3dView)
{
	viewInfo.rendering3dView = rendering3dView;
	// if only more stuff was public ://
	viewInfo.viewSetup = (CViewSetup*)((uintptr_t)rendering3dView + 8);
	viewInfo.frustum = *(VPlane**)(viewInfo.viewSetup + 1); // inward facing
}

// TODO RENAME TO MESHCOMPONENT?
// TODO make sure that I don't create new meshes while any of these are active since they have pointers to a vector
struct SortedMeshElement
{
	MeshUnitWrapper* unitWrapper;
	MeshVertData* vertData; // null for statics
	IMeshWrapper iMeshWrapper;
};

// TODO TTRY MAKING THIS STATIC
std::weak_ordering operator<=>(const SortedMeshElement& a, const SortedMeshElement& b);

void RenderAll(const std::vector<SortedMeshElement>& sortedComponents)
{
	if (sortedComponents.empty())
		return;

	using _it = const SortedMeshElement*;

	_it end = &sortedComponents.back() + 1;
	_it low;
	_it high = &sortedComponents.front();

	while (high != end)
	{
		low = high++;

		if (low->vertData)
		{
			// dynamic - find a suitable range [low,high) that we can batch together
			// TODO can this lambda be shorter? e.g. with bind or something?
			high = std::find_if(low + 1, end, [low](auto& s) { return (*low <=> s) != 0; });
			// copy the vert data references into this temp buffer
			static std::vector<const MeshVertData*> tmp;
			tmp.clear();
			for (auto it = low; it < high; it++)
				tmp.push_back(it->vertData);
			// TODO try using transform?
			// std::transform(low, high, std::back_insert_iterator(tmp), [](const SortedMeshElement& s) { return s.vertData; });

			// batch the whole range
			g_meshBuilderInternal.BeginDynamicMeshCreation(&tmp.front(), &tmp.back() + 1, true);
			IMeshWrapper mw;
			while (mw = g_meshBuilderInternal.GetNextIMeshWrapper(), mw.iMesh)
				low->unitWrapper->Render(mw);
		}
		else
		{
			// statics - there should only be one; don't give it to the mesh builder, we'll render it ourself
			low->unitWrapper->Render(low->iMeshWrapper);
		}
	}
}

void MeshRendererFeature::OnDrawOpaques(CRendering3dView* renderingView)
{
	VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	SetupViewInfo(renderingView);

	static std::vector<SortedMeshElement> sortedMeshes;
	sortedMeshes.clear();

	// go through each component of each mesh unit and add it to the list of it's eligible for rendering

	for (MeshUnitWrapper& meshUnitWrapper : g_meshUnitWrappers)
	{
		if (!meshUnitWrapper.ApplyCallbackAndCalcCamDist(*viewInfo.viewSetup, viewInfo.frustum))
			continue; // the mesh is outside our frustum or the user wants to skip rendering

		if (meshUnitWrapper.callback && meshUnitWrapper.cbInfoOut.colorModulate.a < 1)
			continue; // color modulation makes this mesh unit translucent

		auto shouldRender = [](IMaterial* material)
		{
			return material && !material->GetMaterialVarFlag(MATERIAL_VAR_IGNOREZ)
			       && !material->GetMaterialVarFlag(MATERIAL_VAR_TRANSLUCENT);
		};

		const MeshUnit& meshUnit = meshUnitWrapper.GetMeshUnit();

		if (meshUnit.dynamic)
		{
			for (MeshVertData& vData : meshUnit.dynamicData)
				if (!vData.verts.empty() || shouldRender(vData.material))
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
	RenderAll(sortedMeshes);
}

void MeshRendererFeature::OnDrawTranslucents(CRendering3dView* renderingView)
{
	VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	SetupViewInfo(renderingView);

	static std::vector<SortedMeshElement> sortedMeshes;
	sortedMeshes.clear();

	for (MeshUnitWrapper& meshUnitWrapper : g_meshUnitWrappers)
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
			       || material->GetMaterialVarFlag(MATERIAL_VAR_TRANSLUCENT);
		};

		const MeshUnit& meshUnit = meshUnitWrapper.GetMeshUnit();

		if (meshUnit.dynamic)
		{
			for (MeshVertData& vData : meshUnit.dynamicData)
				if (!vData.verts.empty() || shouldRender(vData.material))
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
		    // TODO check if two meshes overlap from camera's perspective
		    // we already know both objects at least overlap with the frustum, could do something with that
		    if (a.unitWrapper->camDistSqr != b.unitWrapper->camDistSqr)
			    return a.unitWrapper->camDistSqr > b.unitWrapper->camDistSqr;
		    return a < b;
	    });

	RenderAll(sortedMeshes);
}

/**************************************** SPACESHIP ****************************************/

std::weak_ordering operator<=>(const SortedMeshElement& a, const SortedMeshElement& b)
{
	using W = std::weak_ordering;
	// group dynamics together
	if (!a.vertData != !b.vertData)
		return !a.vertData <=> !b.vertData;
	// between dynamics
	if (a.vertData && b.vertData)
	{
		// group the same primitive type together
		if (a.vertData->type != b.vertData->type)
			return a.vertData->type <=> b.vertData->type;
		// group those without a callback together
		if (!a.unitWrapper->callback != !b.unitWrapper->callback)
			return !a.unitWrapper->callback <=> !b.unitWrapper->callback;
		// if both have a callback then just ensure they're different
		if (a.unitWrapper->callback && b.unitWrapper->callback)
			return a.vertData <=> b.vertData;
	}
	else
	{
		// between statics, if both are in the same unit they'll have the same material, callback, colormod, etc.
		if (&a.unitWrapper == &b.unitWrapper)
			return W::equivalent;
	}
	// group the same materials together
	IMaterial* matA = a.vertData ? a.vertData->material : a.iMeshWrapper.material;
	IMaterial* matB = b.vertData ? b.vertData->material : b.iMeshWrapper.material;
	if (matA != matB)
		return matA <=> matB;
	// group the same color mod for a material together, switching color mod seems to be very slow
	if (a.unitWrapper->callback && b.unitWrapper->callback)
	{
		return *reinterpret_cast<int*>(&a.unitWrapper->cbInfoOut.colorModulate)
		       <=> *reinterpret_cast<int*>(&b.unitWrapper->cbInfoOut.colorModulate);
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
		g_meshUnitWrappers.emplace_back(dynamicMesh, callback);
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
		g_meshUnitWrappers.emplace_back(staticMesh.meshPtr, callback);
}

#endif
