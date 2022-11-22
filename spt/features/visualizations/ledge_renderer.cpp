#include "stdafx.h"

#include <map>
#include <algorithm>

#define GAME_DLL
#include "cbase.h"
#include "shareddefs.h"
#include "util_shared.h"
#include "coordsize.h"

#include "renderer\mesh_renderer.hpp"
#include "interfaces.hpp"
#include "spt\sptlib-wrapper.hpp"
#include "spt\features\generic.hpp"
#include "spt\features\ent_props.hpp"
#include "spt\features\playerio.hpp"
#include "spt\utils\ivp_maths.hpp"

#include "iserverentity.h"

ConVar y_spt_draw_ledges("y_spt_draw_ledges", "0");

namespace patterns
{
	PATTERNS(IVP_Compact_Ledge_Solver__get_all_ledges, "5135", "8B 44 24 ?? 8B 48 ?? 03 C8 89 4C 24 ??");
}

struct BigVector
{
	int memsize;
	int n_elems;
	void** elems;
};

struct IvpCompactLedge
{
	int ptOff;
	int _pad0;
	uint _pad1;
	short numTris;
	short _pad2;

	IvpFloatPoint* GetPtArr()
	{
		return (IvpFloatPoint*)((char*)this + ptOff);
	}

	struct IvpCompactTri* GetFirstTri()
	{
		return (IvpCompactTri*)(this + 1);
	}
};

struct IvpCompactEdge
{
	uint startPtIdx : 16;
	uint _pad : 16;

	IvpFloatPoint* GetStartPt(IvpCompactLedge* ledge)
	{
		return &ledge->GetPtArr()[startPtIdx];
	}
};

struct IvpCompactTri
{
	uint _pad;
	IvpCompactEdge edges[3];

	IvpCompactTri* GetNextTri()
	{
		return this + 1;
	}
};

class LedgeRenderer : public FeatureWrapper<LedgeRenderer>
{
	DECL_MEMBER_CDECL(void, IVP_Compact_Ledge_Solver__get_all_ledges, void* surface, BigVector& vec);

	void InitHooks() override
	{
		FIND_PATTERN(vphysics, IVP_Compact_Ledge_Solver__get_all_ledges);
	}

	void LoadFeature() override
	{
		spt_meshRenderer.signal.Connect(this, &LedgeRenderer::OnMeshRenderSignal);
		InitConcommandBase(y_spt_draw_ledges);
	}

	void OnMeshRenderSignal(MeshRendererDelegate& mr)
	{
		if (!y_spt_draw_ledges.GetBool())
			return;

		Vector serverEyes = spt_playerio.m_vecAbsOrigin.GetValue() + spt_playerio.m_vecViewOffset.GetValue();

		Vector dir;
		QAngle ang;
		EngineGetViewAngles((float*)&ang);
		AngleVectors(ang, &dir);

		Ray_t r;
		trace_t tr;
		r.Init(serverEyes, serverEyes + dir * 10000);

		class CTraceIgnorePlayer : public CTraceFilter
		{
			bool ShouldHitEntity(IHandleEntity* pEntity, int contentsMask) override
			{
				return pEntity != spt_entprops.GetPlayer(true);
			}
		} filter;

		interfaces::engineTraceServer->TraceRay(r, MASK_ALL, &filter, &tr);

		mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
		    [&](MeshBuilderDelegate& mb)
		    {
			    Vector clientEyes =
			        spt_entprops.GetPlayerField<Vector>("m_vecAbsOrigin", PropMode::Client).GetValue()
			        + spt_entprops.GetPlayerField<Vector>("m_vecViewOffset", PropMode::Client).GetValue();

			    if (spt_generic.GetCameraOrigin() != clientEyes)
				    mb.AddLine(serverEyes, tr.endpos, {0, 255, 0, 255});

			    mb.AddCross(tr.endpos, 10, {255, 0, 0, 255});
		    }));

		if (!tr.m_pEnt)
			return;

		vcollide_t* vcollide;

		if (tr.hitbox != 0)
		{
			ICollideable* collideable =
			    interfaces::staticPropMgrServer->GetStaticPropByIndex(tr.hitbox - 1);
			vcollide = interfaces::modelInfo->GetVCollide(collideable->GetCollisionModel());
		}
		else
		{
			vcollide = interfaces::modelInfo->GetVCollide(((IServerEntity*)tr.m_pEnt)->GetModelIndex());
		}

		if (!vcollide)
			return;

		CPhysCollide* collide = vcollide->solids[0];

		static CPhysCollide* lastCollide = nullptr;
		static std::vector<StaticMesh> meshes;
		if (collide != lastCollide || (!meshes.empty() && !meshes[0].Valid()))
			meshes.clear();
		if (!meshes.empty())
		{
			for (auto& mesh : meshes)
				mr.DrawMesh(mesh);
			return;
		}

		lastCollide = collide;

		BigVector vec{0, 0, nullptr};

		ORIG_IVP_Compact_Ledge_Solver__get_all_ledges(*(void**)((uintptr_t)collide + 4), vec);

		// we really need to combine the meshes here if we want to view the world
		// ideally the rendering system should handle this for us but whatever
		// (also now I can just use static meshes xd)

		static MeshColor colors[] = {
		    MeshColor::Outline({255, 0, 0, 20}),
		    MeshColor::Outline({0, 255, 0, 20}),
		    MeshColor::Outline({0, 0, 255, 20}),
		    MeshColor::Outline({0, 255, 255, 20}),
		    MeshColor::Outline({255, 0, 255, 20}),
		    MeshColor::Outline({255, 255, 0, 20}),
		    MeshColor::Outline({255, 255, 255, 20}),
		};

		static std::vector<std::tuple<float, IvpCompactLedge*, MeshColor>> sortedLedgeIdxs;
		sortedLedgeIdxs.clear();

		IvpFloatPoint ivpCamPos{spt_generic.GetCameraOrigin(), true};

		for (int i = 0; i < vec.n_elems; i++)
		{
			Vector mins{INFINITY};
			Vector maxs{-INFINITY};
			IvpCompactLedge* ledge = (IvpCompactLedge*)vec.elems[i];
			IvpCompactTri* tri = ledge->GetFirstTri();
			for (int j = 0; j < ledge->numTris; j++)
			{
				for (int k = 0; k < 3; k++)
				{
					IvpCompactEdge* edge = &tri->edges[k];
					IvpFloatPoint* pt = edge->GetStartPt(ledge);
					Vector* v = (Vector*)pt;
					VectorMin(*v, mins, mins);
					VectorMax(*v, maxs, maxs);
				}
				tri = tri->GetNextTri();
			}
			float dist = CalcDistanceToAABB(mins, maxs, *(Vector*)&ivpCamPos);
			sortedLedgeIdxs.emplace_back(dist,
			                             (IvpCompactLedge*)vec.elems[i],
			                             colors[i % _countof(colors)]);
		}
		// add a dummy null ledge so we can render the last batch
		sortedLedgeIdxs.emplace_back(INFINITY, nullptr, MeshColor::Face({0, 0, 0, 0}));

		std::sort(sortedLedgeIdxs.begin(),
		          sortedLedgeIdxs.end(),
		          [](auto& a, auto& b) { return std::get<0>(a) < std::get<0>(b); });

		static std::vector<Vector> scratch;
		static std::vector<std::pair<size_t, MeshColor>> scratchCheckpoints;
		scratch.clear();
		scratchCheckpoints.clear();

		for (const auto& [_, ledge, ledgeColor] : sortedLedgeIdxs)
		{
			IvpCompactTri* tri = ledge ? ledge->GetFirstTri() : nullptr;
			scratchCheckpoints.emplace_back(scratch.size(), ledgeColor);

			// flush the scratch buffer if we have too many tris
			if (!ledge || (scratch.size() + ledge->numTris) * 6 > 32768)
			{
				// batch render all ledges together as a single mesh
				meshes.push_back(spt_meshBuilder.CreateStaticMesh(
				    [&](MeshBuilderDelegate& mb)
				    {
					    for (size_t c = 0; c < scratchCheckpoints.size() - 1; c++)
					    {
						    auto [c1, color] = scratchCheckpoints[c];
						    auto [c2, _] = scratchCheckpoints[c + 1];
						    mb.AddTris(scratch.data() + c1, (c2 - c1) / 3, color);
					    }
				    },
				    {ZTEST_LINES, CullType::Reverse}));
				if (!ledge)
					break; // break if on (last) dummy ledge
				scratch.clear();
				scratchCheckpoints.clear();
			}
			for (int j = 0; j < ledge->numTris; j++)
			{
				for (int k = 0; k < 3; k++)
				{
					IvpCompactEdge* edge = &tri->edges[k];
					IvpFloatPoint* pt = edge->GetStartPt(ledge);

					Vector v = *(Vector*)pt;
					v /= METERS_PER_INCH;
					std::swap(v[1], v[2]);
					v.z *= -1;
					scratch.push_back(v);
				}
				tri = tri->GetNextTri();
			}
		}

		delete vec.elems;

		for (auto& mesh : meshes)
			mr.DrawMesh(mesh);
	}
};

static LedgeRenderer ledgeRendererFeature;
