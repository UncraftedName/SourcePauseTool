/*
* This file contains the implementations for all 3 mesh renderers! Their purpose is somewhat similar to the 3
* mesh builders:
* - MeshRendererFeature (spt_meshRenderer)
* - MeshRendererDelegate
* - MeshRendererInternal (g_meshRendererInternal)
* The MeshBuilderFeature contains hooks and the mesh render signal, but is otherwise effectively stateless. At the
* start of every frame, it will signal all users and give them a stateless delegate. The user then gives this
* delegate dynamic or static meshes created with the mesh builder, and the delegate edits the state of the
* internal renderer.
* 
* At the start of a frame, we'll get a signal from spt_overlay in which we do cleanup from the previous frame,
* then we signal the users. The meshes from the users will get wrapped and queued for drawing later. We take
* advantage of two passes the game does: DrawOpaques and DrawTranslucents. Both are called for each view, and you
* may get a different view for each portal, saveglitch overlay, etc. We abstract this away from the user - when
* the user calls DrawMesh() we defer the drawing until those two passes. When rendering opaques, we can render the
* meshes in any order, but the same is not true for translucents. In that case we must figure out our own render
* order. Nothing we do will be perfect - but a good enough solution for many cases is to approximate the distance
* to each mesh from the camera as a single value and sort based on that.
* 
* This system would not exist without the absurd of times mlugg has helped me while making it; send him lots of
* love, gifts, and fruit baskets.
*/

#include "stdafx.hpp"

#include "..\mesh_renderer.hpp"
#include "mesh_renderer_internal.hpp"
#include "imesh_builder.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <algorithm>
#include <numeric>

#include "internal_defs.hpp"
#include "interfaces.hpp"
#include "signals.hpp"

#include "spt/features/visualizations/imgui/imgui_interface.hpp"

ConVar y_spt_draw_mesh_debug(
    "y_spt_draw_mesh_debug",
    "0",
    FCVAR_CHEAT | FCVAR_DONTRECORD,
    "Draws the AABB and position metric of all meshes, uses the following colors:\n"
    "   - red: static mesh\n"
    "   - blue: dynamic mesh\n"
    "   - yellow: dynamic mesh with a callback\n"
    "   - red cross: the position metric to a translucent mesh, used to determine the render order\n"
    "   - green: fused opaque dynamic meshes (only available with a cvar value of 2)\n"
    "   - light green: fused translucent dynamic meshes (only available with a cvar value of 2)");

CON_COMMAND_F(y_spt_destroy_all_static_meshes,
              "Destroy all static meshes created with the mesh builder, used for debugging",
              FCVAR_DONTRECORD)
{
	int count = spt_meshRenderer.DestroyAllStaticMeshes();
	Msg("Destroyed %d static mesh%s\n", count, count == 1 ? "" : "es");
};

#define DEBUG_COLOR_STATIC_MESH _COLOR(150, 20, 10, 255)
#define DEBUG_COLOR_DYNAMIC_MESH _COLOR(0, 0, 255, 255)
#define DEBUG_COLOR_DYNAMIC_MESH_WITH_CALLBACK _COLOR(255, 150, 50, 255)
#define DEBUG_COLOR_FUSED_DYNAMIC_MESH_OPAQUE _COLOR(0, 255, 0, 255)
#define DEBUG_COLOR_FUSED_DYNAMIC_MESH_TRANSLUCENT _COLOR(150, 255, 200, 255)
#define DEBUG_COLOR_CROSS _COLOR(255, 0, 0, 255)

/**************************************** MESH RENDERER FEATURE ****************************************/

namespace patterns
{
	PATTERNS(CRendering3dView__DrawOpaqueRenderables,
	         "5135-portal1",
	         "55 8D 6C 24 8C 81 EC 94 00 00 00",
	         "7462488-portal1",
	         "55 8B EC 81 EC 80 00 00 00 8B 15 ?? ?? ?? ??");
	PATTERNS(CRendering3dView__DrawTranslucentRenderables,
	         "5135-portal1",
	         "55 8B EC 83 EC 34 53 8B D9 8B 83 94 00 00 00 8B 13 56 8D B3 94 00 00 00",
	         "5135-hl2",
	         "55 8B EC 83 EC 34 83 3D ?? ?? ?? ?? 00",
	         "1910503-portal1",
	         "55 8B EC 81 EC 9C 00 00 00 53 56 8B F1 8B 86 E8 00 00 00 8B 16 57 8D BE E8 00 00 00",
	         "7462488-portal1",
	         "55 8B EC 81 EC A0 00 00 00 53 8B D9",
	         "7467727-hl2",
	         "55 8B EC 81 EC A0 00 00 00 83 3D ?? ?? ?? ?? 00");
	PATTERNS(CSkyBoxView__DrawInternal,
	         "EZ2-1.0.0+original",
	         "55 8B EC 81 EC 28 01 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 ?? 8B 45 ??",
	         "5135-portal1",
	         "83 EC 28 53 56 8B F1 8B 0D ?? ?? ?? ??",
	         "7467727-hl2",
	         "55 8B EC 83 EC 24 53 56 57 8B F9 8B 0D ?? ?? ?? ??");
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
	HOOK_FUNCTION(client, CSkyBoxView__DrawInternal);
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

	g_meshMaterialMgr.Load();
}

void MeshRendererFeature::LoadFeature()
{
	if (!signal.Works)
		return;

	RenderViewPre_Signal.Connect(this, &MeshRendererFeature::OnRenderViewPre_Signal);
	InitConcommandBase(y_spt_draw_mesh_debug);
	InitCommand(y_spt_destroy_all_static_meshes);
	SptImGuiGroup::Dev_Mesh.RegisterUserCallback(MeshRendererFeature::ImGuiCallback);
}

void MeshRendererFeature::UnloadFeature()
{
	signal.Clear();
	rendererUnloadMutex.lock();
	frameData.reset();
	rendererUnloadMutex.unlock();
	g_meshMaterialMgr.Unload();
	StaticMesh::DestroyAll();
}

std::unique_ptr<MeshRendererFeature::FrameDataImpl> MeshRendererFeature::frameData;

void MeshRendererFeature::OnRenderViewPre_Signal(void* thisptr, CViewSetup* cameraView)
{
	// ensure we only run once per frame
	if (spt_overlay.renderingOverlay || !spt_meshRenderer.signal.Works)
		return;

	SPT_VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	persist.frameNum++;

	std::unique_lock lk{rendererUnloadMutex};
	size_t nBytesAllocedLast = frameData ? frameData->outerCountingResource.nTotalBytesAlloc.load() : 0;
	frameData = std::make_unique<FrameDataImpl>(nBytesAllocedLast);
	frameData->renderer.acceptUserData = true;
	MeshRendererDelegate renderDelgate{};
	signal(renderDelgate);
	frameData->renderer.acceptUserData = false;
}

void MeshRendererFeature::ImGuiCallback()
{
	extern ConVar y_spt_draw_mesh_examples;
	SptImGui::CvarCheckbox(y_spt_draw_mesh_examples, "Draw mesh examples");

	static int lastDestroyedCount = 0;
	static double lastDestroyedTime = -666.f;
	if (SptImGui::CmdButton("Destroy all static meshes", y_spt_destroy_all_static_meshes_command))
	{
		lastDestroyedCount = StaticMesh::DestroyAll();
		lastDestroyedTime = ImGui::GetTime();
	}
	bool emph = ImGui::GetTime() - lastDestroyedTime <= .5;
	ImGui::TextColored(emph ? ImVec4{.8f, .8f, .3f, 1.f} : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
	                   "(%d mesh%s destroyed last time)",
	                   lastDestroyedCount,
	                   lastDestroyedCount == 1 ? "" : "es");

	const char* const opts[] = {
	    "Disabled",
	    "Draw AABB for meshes",
	    "Draw AABB for meshes & fused meshes",
	};
	SptImGui::CvarCombo(y_spt_draw_mesh_debug, "##draw_mesh_debug", opts, ARRAYSIZE(opts));

	size_t totalVerts = 0;
	size_t totalIndices = 0;
	size_t totalDynamics = 0;
	size_t totalStatics = 0;
	size_t nBytesAlloced = 0;
	size_t nBytesUsed = 0;

	std::unique_lock lk{rendererUnloadMutex};
	if (frameData)
	{
		totalDynamics = frameData->renderer.stats.nDynamicsThisFrame;
		totalStatics = frameData->renderer.stats.nStaticsThisFrame;
		nBytesAlloced = frameData->outerCountingResource.nTotalBytesAlloc.load();
		nBytesUsed = frameData->innerCountingResource.nTotalBytesAlloc.load();

		for (const auto& unit : frameData->renderer.dynamicUnits)
		{
			for (const auto& component : unit.componentBufs)
			{
				totalVerts += component.verts.size();
				totalIndices += component.indices.size();
			}
		}
	}

	ImGui::Text("%3u dynamic units queued this frame: %6u verts %6u indices",
	            totalDynamics,
	            totalVerts,
	            totalIndices);
	ImGui::Text("%3u static mesh units queued this frame", totalStatics);
	ImGui::Text("%6u bytes allocated this frame (%6u used)", nBytesAlloced, nBytesUsed);
}

int MeshRendererFeature::CurrentPortalRenderDepth() const
{
	std::unique_lock lk{rendererUnloadMutex};
	if (!frameData)
		return 0;
	return MAX(0, (int)frameData->renderer.debugMeshViews.size() - 1);
}

int MeshRendererFeature::DestroyAllStaticMeshes() const
{
	return StaticMesh::DestroyAll();
}

IMPL_HOOK_THISCALL(MeshRendererFeature, void, CRendering3dView__DrawOpaqueRenderables, CRendering3dView*, int param)
{
	// HACK - param is a bool in SSDK2007 and enum in SSDK2013
	// render order shouldn't matter here
	ORIG_CRendering3dView__DrawOpaqueRenderables(thisptr, param);
	// TODO - in all of the calls to the renderer functions, I should set the default memory resource to the null one
	std::unique_lock lk{rendererUnloadMutex};
	MbParanoidAllocScope scope{};
	if (frameData)
		frameData->renderer.OnDrawOpaques(thisptr);
}

IMPL_HOOK_THISCALL(MeshRendererFeature,
                   void,
                   CRendering3dView__DrawTranslucentRenderables,
                   CRendering3dView*,
                   bool inSkybox,
                   bool shadowDepth)
{
	// render order matters here, render our stuff on top of other translucents
	ORIG_CRendering3dView__DrawTranslucentRenderables(thisptr, inSkybox, shadowDepth);
	std::unique_lock lk{rendererUnloadMutex};
	MbParanoidAllocScope scope{};
	if (frameData)
		frameData->renderer.OnDrawTranslucents(thisptr);
}

#ifdef SSDK2007
IMPL_HOOK_THISCALL(MeshRendererFeature,
                   void,
                   CSkyBoxView__DrawInternal,
                   void*,
                   view_id_t id,
                   bool bInvoke,
                   ITexture* pRenderTarget)
{
	{
		std::unique_lock lk{rendererUnloadMutex};
		if (frameData)
			frameData->renderer.renderingSkyBox = true;
	}
	ORIG_CSkyBoxView__DrawInternal(thisptr, id, bInvoke, pRenderTarget);
	{
		std::unique_lock lk{rendererUnloadMutex};
		if (frameData)
			frameData->renderer.renderingSkyBox = false;
	}
}
#else
IMPL_HOOK_THISCALL(MeshRendererFeature,
                   void,
                   CSkyBoxView__DrawInternal,
                   void*,
                   view_id_t id,
                   bool bInvoke,
                   ITexture* pRenderTarget,
                   ITexture* pDepthTarget)
{
	{
		std::unique_lock lk{rendererUnloadMutex};
		if (frameData)
			frameData->renderer.renderingSkyBox = true;
	}
	ORIG_CSkyBoxView__DrawInternal(thisptr, id, bInvoke, pRenderTarget, pDepthTarget);
	{
		std::unique_lock lk{rendererUnloadMutex};
		if (frameData)
			frameData->renderer.renderingSkyBox = false;
	}
}
#endif

/**************************************** MESH UNIT WRAPPER ****************************************/

// returns true if this unit should be rendered
std::optional<MbUnitRenderData> MbMeshUnit::GetRenderData() const
{
	Assert(spt_meshRenderer.frameData);

	auto& internalRenderer = spt_meshRenderer.frameData->renderer;
	auto& viewInfo = internalRenderer.viewInfo;

	MbUnitRenderData ret{
	    .posInfo =
	        staticUnit ? staticUnit->posInfo : internalRenderer.GetDynamicMeshFromToken(dynamicToken).posInfo,
	    .hasCallback = !!callback,
	};
	auto& posInfo = ret.posInfo;

	if (callback)
	{
		// apply callback

		CallbackInfoIn infoIn = {
		    *viewInfo.viewSetup,
		    posInfo,
		    spt_meshRenderer.CurrentPortalRenderDepth(),
		    spt_overlay.renderingOverlay,
		};

		callback(infoIn, ret.cbInfoOut);

		if (ret.cbInfoOut.skipRender || ret.cbInfoOut.colorModulate.a == 0)
			return std::nullopt;
		TransformAABB(ret.cbInfoOut.mat, posInfo.mins, posInfo.maxs, posInfo.mins, posInfo.maxs);
	}

	// do frustum check

	auto& frustum = viewInfo.frustum;
	for (int i = 0; i < 6; i++)
		if (BoxOnPlaneSide((float*)&posInfo.mins, (float*)&posInfo.maxs, &frustum[i]) == 2)
			return std::nullopt;

	// calc camera to mesh "distance"

	CalcClosestPointOnAABB(posInfo.mins, posInfo.maxs, viewInfo.viewSetup->origin, ret.camDistSqrTo);
	if (viewInfo.viewSetup->origin == ret.camDistSqrTo)
		ret.camDistSqrTo = (posInfo.mins + posInfo.maxs) / 2.f; // if inside cube, use center idfk
	ret.camDistSqr = viewInfo.viewSetup->origin.DistToSqr(ret.camDistSqrTo);

	return ret;
}

void MeshRendererInternal::Render(const IMeshWrapper& mw, const MbUnitRenderData& renderData)
{
	if (!mw.iMesh)
		return;
	CMatRenderContextPtr context{interfaces::materialSystem};
	if (renderData.hasCallback)
	{
		color32 cMod = renderData.cbInfoOut.colorModulate;
		mw.material->ColorModulate(cMod.r / 255.f, cMod.g / 255.f, cMod.b / 255.f);
		mw.material->AlphaModulate(cMod.a / 255.f);
		context->MatrixMode(MATERIAL_MODEL);
		context->PushMatrix();
		context->LoadMatrix(renderData.cbInfoOut.mat);
	}
	else
	{
		mw.material->ColorModulate(1, 1, 1);
		mw.material->AlphaModulate(1);
	}
	context->Bind(mw.material);
	mw.iMesh->Draw();
	if (mw.dynamic)
		context->Flush(); // dynamic meshes seem to be slightly glitchy, this sometimes fixes that
	if (renderData.hasCallback)
		context->PopMatrix();
}

/**************************************** MESH RENDER FEATURE ****************************************/

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
	// if there's any use for rendering stuff in sky boxes in the future we can add this to the callback info
	if (renderingSkyBox)
		return;

	SPT_VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	SetupViewInfo(renderingView);

	// add a new debug view, the corresponding pop is at the end of DrawTranslucents
	debugMeshViews.emplace_back(mr);

	std::pmr::list<MbStagingComponent> components{&mr};
	CollectRenderableComponents(components, true);

	std::pmr::vector<std::reference_wrapper<MbStagingComponent>> sortedComponents{
	    components.begin(),
	    components.end(),
	    &mr,
	};

	std::ranges::stable_sort(sortedComponents, std::less{});

	DrawAll(sortedComponents, y_spt_draw_mesh_debug.GetBool(), true);
	components.clear();
}

void MeshRendererInternal::OnDrawTranslucents(CRendering3dView* renderingView)
{
	if (renderingSkyBox)
		return;

	SPT_VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);
	SetupViewInfo(renderingView);

	std::pmr::list<MbStagingComponent> components{&mr};
	CollectRenderableComponents(components, false);

	std::pmr::vector<std::reference_wrapper<MbStagingComponent>> sortedComponents{
	    components.begin(),
	    components.end(),
	    &mr,
	};

	/*
	* Translucent meshes must be sorted by distance first which makes them not as good for fusing. In theory
	* there should be a way to ignore the position metric if the mesh units don't overlap in screen space, but
	* I couldn't get that to work (maybe because such a comparison would not be transitive?). I think this sort
	* should be stable because I want components from the same unit to remain the same order relative to each
	* other in case <=> returns equivalent. I think this could be useful if I decide to spill components in case
	* of overflow.
	*/
	std::ranges::stable_sort(sortedComponents,
	                         [](const MbStagingComponent& a, const MbStagingComponent& b)
	                         {
		                         MaterialRef matA = a.component.GetMaterial();
		                         MaterialRef matB = b.component.GetMaterial();
		                         auto ta = std::make_tuple(matA,
		                                                   matA->GetMaterialVarFlag(MATERIAL_VAR_IGNOREZ),
		                                                   a.renderData.camDistSqr);
		                         auto tb = std::make_tuple(matB,
		                                                   matB->GetMaterialVarFlag(MATERIAL_VAR_IGNOREZ),
		                                                   b.renderData.camDistSqr);
		                         if (auto cmp = ta <=> tb; cmp != std::weak_ordering::equivalent)
			                         return cmp < 0;
		                         return a < b;
	                         });

	DrawAll(sortedComponents, y_spt_draw_mesh_debug.GetBool(), false);

	if (y_spt_draw_mesh_debug.GetBool())
	{
		AddDebugCrosses(components);
		DrawDebugMeshesForCurrentView(); // draw all translucent!!! debug meshes
	}
	debugMeshViews.pop_back();
}

void MeshRendererInternal::CollectRenderableComponents(std::pmr::list<MbStagingComponent>& components, bool opaques)
{
	SPT_VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);

	// go through all components of all queued meshes and return those that are eligable for rendering right now
	for (const MbMeshUnit& unit : queuedUnits)
	{
		std::optional<MbUnitRenderData> renderData = unit.GetRenderData();
		if (!renderData)
			continue; // the mesh is outside our frustum or the user wants to skip rendering

		if (unit.callback && opaques && renderData->cbInfoOut.colorModulate.a < 1)
			continue; // color modulation forces all meshes in this unit to be translucent

		auto shouldRender = [&renderData, opaques](IMaterial* material)
		{
			if (!material)
				return false;

			if (renderData->hasCallback)
				if (renderData->cbInfoOut.colorModulate.a < 255)
					return !opaques; // callback changed alpha component, make translucent if < 1 otherwise opaque

			bool opaqueMaterial = !material->GetMaterialVarFlag(MATERIAL_VAR_IGNOREZ)
			                      && !material->GetMaterialVarFlag(MATERIAL_VAR_VERTEXALPHA);

			return opaques == opaqueMaterial;
		};

		// TODO - I'm copying the mesh render data for each component, I should plop it into a separate (linked) list

		if (unit.IsDynamic())
		{
			for (auto& component : GetDynamicMeshFromToken(unit.dynamicToken).componentBufs)
				if (shouldRender(component.GetMaterial()))
					components.emplace_back(MbComponent{component}, *renderData);
		}
		else
		{
			for (auto& component : unit.staticUnit->meshes)
				if (shouldRender(component.material))
					components.emplace_back(MbComponent{component}, *renderData);
		}
	}
}

void MeshRendererInternal::AddDebugCrosses(std::pmr::list<MbStagingComponent>& components)
{
	auto& debugDescs = debugMeshViews.back().descs;
	for (auto& comp : components)
	{
		const MeshPositionInfo& posInfo = comp.renderData.posInfo;
		float maxDiameter = VectorMaximum(posInfo.maxs - posInfo.mins);
		const float smallest = 1, biggest = 15, falloff = 100;
		// scale cross by the AABB size, plot this bad boy in desmos as a function of maxBoxDim
		float size = -falloff * (biggest - smallest) / (maxDiameter + falloff) + biggest;

		debugDescs.emplace_back(DebugCross{comp.renderData.camDistSqrTo, size}, DEBUG_COLOR_CROSS);
	}
}

void MeshRendererInternal::DrawDebugMeshesForCurrentView()
{
	/*
	* This is like a miniature version of what the whole renderer does, but we don't have to worry about drawing
	* the meshes in DrawOpaques() and DrawTranslucents(). We create the dynamic debug meshes for this view, wrap
	* them up in unit wrappers, sort them (which should do nothing since they all use the same materials), and
	* batch them together.
	*/

	auto& debugDescs = debugMeshViews.back().descs;
	std::pmr::vector<MbStagingComponent> components{&mr};
	components.reserve(debugDescs.size());

	acceptUserData = true;
	for (auto& debugDesc : debugDescs)
	{
		DynamicMesh dynMesh = spt_meshBuilder.CreateDynamicMesh(
		    [&](MeshBuilderDelegate& mb)
		    {
			    if (auto box = std::get_if<DebugBox>(&debugDesc.desc))
			    {
				    mb.AddBox(vec3_origin,
				              box->mins,
				              box->maxs,
				              vec3_angle,
				              {C_WIRE(debugDesc.color), false, false});
			    }
			    else if (auto cross = std::get_if<DebugCross>(&debugDesc.desc))
			    {
				    mb.AddCross(cross->crossPos, cross->size, {debugDesc.color, false});
			    }
			    else
			    {
				    __debugbreak();
			    }
		    });

		stats.nDynamicsThisFrame++;
		MbMeshUnit unit{dynMesh, nullptr};
		if (auto renderData = unit.GetRenderData(); renderData)
			for (auto& component : GetDynamicMeshFromToken(unit.dynamicToken).componentBufs)
				components.emplace_back(MbComponent{component}, *renderData);
	}
	acceptUserData = false;

	std::pmr::vector<std::reference_wrapper<MbStagingComponent>> sortedComponents{
	    components.begin(),
	    components.end(),
	    &mr,
	};
	std::ranges::stable_sort(sortedComponents, std::less{});
	DrawAll(sortedComponents, false, true);
}

// move define to imesh builder
#define MB_FUSE_MESHES 1

void MeshRendererInternal::DrawAll(std::span<const std::reference_wrapper<MbStagingComponent>> span,
                                   bool addDebugMeshes,
                                   bool opaques)
{
	SPT_VPROF_BUDGET(__FUNCTION__, VPROF_BUDGETGROUP_MESH_RENDERER);

	/*
	* We create a subspan of fullSpan: compatSpan. Our goal is to give spans to the builder that can be
	* fused together. Static meshes can't be fused (they're already IMesh* objects), so we render those
	* one at a time. For dynamic meshes, we find the first element such that (first <=> elem) != 0. We
	* rely on operator<=> to tell us if two components are eligable for fusing.
	*/

	while (!span.empty())
	{
		if (span.front().get().IsDynamic())
		{
#if MB_FUSE_MESHES
			auto it =
			    std::ranges::find_if(span,
			                         [span](auto& staging) { return (span.front() <=> staging) != 0; });
#else
			auto it = span.begin() + 1;
#endif

			std::span attemptFuseSpan(span.begin(), it);
			AssertMsg(!attemptFuseSpan.empty(),
			          "spt: operator<=> does not return equivalent for two of the same staging component");

			MbIMeshBuilder builder{
			    attemptFuseSpan,
			    [](const std::reference_wrapper<MbStagingComponent>& staging) -> auto&
			    { return staging.get().component.GetDynamic(); },
			    true,
			};
			while (builder.FuseNext())
			{
				auto& [imw, fusedSpan] = builder.GetCurrent();
				Render(imw, fusedSpan.front().get().renderData);
				if (addDebugMeshes)
					AddDebugBoxesForFusedDynamic(fusedSpan, opaques);
			}
			span = span.subspan(attemptFuseSpan.size());
		}
		else
		{
			// a single static mesh
			auto component = span.front();
			Render(component.get().component.GetStatic(), component.get().renderData);
			if (addDebugMeshes)
				AddDebugBoxForStatic(component);
			span = span.subspan(1);
		}
	}
}

void MeshRendererInternal::AddDebugBoxesForFusedDynamic(
    std::span<const std::reference_wrapper<MbStagingComponent>> span,
    bool opaques)
{
	auto& debugDescs = debugMeshViews.back().descs;

	Vector fusedMins{std::numeric_limits<float>::infinity()};
	Vector fusedMaxs = -fusedMins;
	for (auto staging : span)
	{
		auto& posInfo = staging.get().renderData.posInfo;
		VectorMin(posInfo.mins, fusedMins, fusedMins);
		VectorMax(posInfo.maxs, fusedMaxs, fusedMaxs);

		debugDescs.emplace_back(DebugBox{posInfo.mins - Vector{1.f}, posInfo.maxs + Vector{1.f}},
		                        staging.get().renderData.hasCallback ? DEBUG_COLOR_DYNAMIC_MESH_WITH_CALLBACK
		                                                             : DEBUG_COLOR_DYNAMIC_MESH);
	}
	if (span.size() > 1 && y_spt_draw_mesh_debug.GetInt() >= 2)
	{
		debugDescs.emplace_back(DebugBox{fusedMins - Vector{opaques ? 2.5f : 2.f},
		                                 fusedMaxs + Vector{opaques ? 2.5f : 2.f}},
		                        opaques ? DEBUG_COLOR_FUSED_DYNAMIC_MESH_OPAQUE
		                                : DEBUG_COLOR_FUSED_DYNAMIC_MESH_TRANSLUCENT);
	}
}

void MeshRendererInternal::AddDebugBoxForStatic(const MbStagingComponent& component)
{
	auto& descs = debugMeshViews.back().descs;
	auto& posInfo = component.renderData.posInfo;
	descs.emplace_back(DebugBox{posInfo.mins - Vector{1.f}, posInfo.maxs + Vector{1.f}}, DEBUG_COLOR_STATIC_MESH);
}

/**************************************** SPACESHIP ****************************************/

/*
* This operator has three purposes:
* 1) Determine if two components may be fused (must evaluate to std::weak_ordering::equivalent)
* 2) Sort lists of components so that consecutive elements are eligable for fusion
* 3) Order components in a way to reduce overhead (e.g. grouping the same color modulation together)
* 
* For example, using this operator to sort a list of components will order them sort of like this:
* 
*                              V 0-3 can be fused V     V 4-8 can be fused V    static V      static V
* std::vector<MeshComponent>: [0]---[1]---[2]----[3]---[4]---[5]---[7]----[8]---[9]---[10]---[11]---[12]
*                                                            static (can't fuse) ^     static ^
* 
* After sorting, the renderer will iterate over the spans [0,4), [4,9), [9,10), [10,11), [11,12), [12,13) and
* render them. The first two spans have dynamic meshes, so they would be given to the builder to attempt fusion.
* The last four are statics; they would be ordererd in a way to minimize context switching e.g. [9], [10], [11]
* would have the same material & color modulation.
*/
std::weak_ordering operator<=>(const MbStagingComponent& a, const MbStagingComponent& b)
{
	constexpr auto eqv = std::weak_ordering::equivalent;

	if (&a == &b)
		return eqv;

	// group dynamics together
	if (auto cmp = a.component.IsDynamic() <=> b.component.IsDynamic(); cmp != eqv)
		return cmp;

	if (a.component.IsDynamic())
	{
		// between dynamics group by whether we have a callback, then by primitive type, then by unit
		auto ta = std::make_tuple(a.renderData.hasCallback,
		                          a.component.GetDynamic().primType,
		                          a.component.GetDynamic().matType);
		auto tb = std::make_tuple(b.renderData.hasCallback,
		                          b.component.GetDynamic().primType,
		                          b.component.GetDynamic().matType);
		if (auto cmp = ta <=> tb; cmp != eqv)
			return cmp;
	}
	else
	{
		// statics - group the same materials together
		if (auto cmp = a.component.GetMaterial() <=> b.component.GetMaterial(); cmp != eqv)
			return cmp;
	}

	// group the same color mod for a material together, switching color mod seems to be very slow
	auto ta = std::make_tuple(a.renderData.hasCallback,
	                          *reinterpret_cast<const int*>(&a.renderData.cbInfoOut.colorModulate));
	auto tb = std::make_tuple(b.renderData.hasCallback,
	                          *reinterpret_cast<const int*>(&b.renderData.cbInfoOut.colorModulate));
	if (auto cmp = ta <=> tb; cmp != eqv)
		return cmp;

	// for dynamics, assume that each callback is different unless we're dealing with components from a single unit
	if (a.component.IsDynamic() && a.renderData.hasCallback)
	{
		if (auto cmp = &a.renderData <=> &b.renderData; cmp != eqv)
			return cmp;
	}

	return eqv;
}

/**************************************** MESH RENDERER DELEGATE ****************************************/

void MeshRendererDelegate::DrawMesh(const DynamicMesh& dynamicMesh, const RenderCallback& callback)
{
	if (!spt_meshRenderer.frameData)
	{
		AssertMsg(0, "spt: Frame data was not initialized");
		return;
	}
	auto& internalRenderer = spt_meshRenderer.frameData->renderer;
	if (!internalRenderer.acceptUserData)
	{
		AssertMsg(0, "spt: Meshes can only be drawn in MeshRenderSignal!");
		return;
	}
	if (dynamicMesh.createdFrame != spt_meshRenderer.FrameNum())
	{
		AssertMsg(0, "spt: Attempted to reuse a dynamic mesh between frames");
		Warning("spt: Can only draw dynamic meshes on the frame they were created!\n");
		return;
	}
	if (!internalRenderer.GetDynamicMeshFromToken(dynamicMesh).IsEmpty())
	{
		internalRenderer.queuedUnits.emplace_back(dynamicMesh, callback);
		internalRenderer.stats.nDynamicsThisFrame++;
	}
}

void MeshRendererDelegate::DrawMesh(const StaticMesh& staticMesh, const RenderCallback& callback)
{
	if (!spt_meshRenderer.frameData)
	{
		AssertMsg(0, "spt: Frame data was not initialized");
		return;
	}
	auto& internalRenderer = spt_meshRenderer.frameData->renderer;
	if (!internalRenderer.acceptUserData)
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
	if (!staticMesh.meshPtr->IsEmpty())
	{
		internalRenderer.queuedUnits.emplace_back(staticMesh.meshPtr, callback);
		internalRenderer.stats.nStaticsThisFrame++;
	}
}

#endif
