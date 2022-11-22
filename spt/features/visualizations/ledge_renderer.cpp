#include "stdafx.h"

#include "renderer\mesh_renderer.hpp"
#include "interfaces.hpp"
#include "spt\sptlib-wrapper.hpp"
#include "spt\features\generic.hpp"
#include "spt\features\ent_props.hpp"
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

		Vector pos, dir;
		QAngle ang;
		pos = spt_generic.GetCameraOrigin();
		EngineGetViewAngles((float*)&ang);
		AngleVectors(ang, &dir);

		Ray_t r;
		trace_t tr;
		r.Init(pos, pos + dir * 10000);

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
			    mb.AddLine(tr.startpos, tr.endpos, {0, 255, 0, 255});
			    mb.AddCross(tr.endpos, 10, {255, 0, 0, 255});
		    }));

		if (!tr.m_pEnt)
			return;

		// int off = spt_entprops.GetFieldOffset("CBaseEntity", "m_nModelIndex", false);
		// int modelIdx = *((int*)tr.m_pEnt + off / 4);
		vcollide_t* vcollide = interfaces::modelInfo->GetVCollide(((IServerEntity*)tr.m_pEnt)->GetModelIndex());
		if (!vcollide)
			return;

		CPhysCollide* collide = vcollide->solids[0];
		BigVector vec{0, 0, nullptr};

		ORIG_IVP_Compact_Ledge_Solver__get_all_ledges(*(void**)((uintptr_t)collide + 4), vec);

		MeshColor colors[] = {
		    MeshColor::Outline({255, 0, 0, 20}),
		    MeshColor::Outline({0, 255, 0, 20}),
		    MeshColor::Outline({0, 0, 255, 20}),
		    MeshColor::Outline({0, 255, 255, 20}),
		    MeshColor::Outline({255, 0, 255, 20}),
		    MeshColor::Outline({255, 255, 0, 20}),
		    MeshColor::Outline({255, 255, 255, 20}),
		};

		static std::vector<Vector> vecScratch;

		for (int i = 0; i < vec.n_elems; i++)
		{
			void* ledge = vec.elems[i];
			short num_triangles = *((short*)ledge + 6);

			vecScratch.clear();
			vecScratch.resize(num_triangles);

			int cPointOff = *(int*)ledge;
			IvpFloatPoint* pointArray = (IvpFloatPoint*)((uintptr_t)ledge + cPointOff);

			for (short j = 0; j < num_triangles; j++)
			{
				uint* tri = (uint*)ledge + 4;
				uint* edges = tri + 1;
				for (int k = 0; k < 3; k++)
				{
					uint startPtIdx = edges[k] >> 16; // upper 16 bits
					IvpFloatPoint pt = pointArray[startPtIdx];

					Vector v = *reinterpret_cast<Vector*>(&pt);
					v /= METERS_PER_INCH;
					std::swap(v[1], v[2]);
					v.z *= -1;
					vecScratch.push_back(v);
				}
			}
			MeshColor color = colors[i % _countof(colors)];

			mr.DrawMesh(
			    spt_meshBuilder.CreateDynamicMesh([&](MeshBuilderDelegate& mb)
			                                      { mb.AddTris(vecScratch.data(), num_triangles, color); },
			                                      {ZTEST_FACES | ZTEST_LINES, CullType::Reverse}));
		}

		delete vec.elems;
	}
};

static LedgeRenderer ledgeRendererFeature;
