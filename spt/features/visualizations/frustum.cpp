#include "stdafx.h"

#include "renderer\mesh_renderer.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "spt\features\ent_props.hpp"
#include "spt\features\playerio.hpp"
#include "spt\sptlib-wrapper.hpp"

ConVar y_spt_draw_frustum("y_spt_draw_frustum",
                          "0",
                          FCVAR_CHEAT | FCVAR_DONTRECORD,
                          "Draw what the frustum would look like for a player");

ConVar y_spt_draw_frustum_far_plane("y_spt_draw_frustum_far_plane",
                                    "16000",
                                    FCVAR_CHEAT | FCVAR_DONTRECORD,
                                    "<3 Hi Imanex");

class FrustumFeature : public FeatureWrapper<FrustumFeature>
{
	void LoadFeature() override
	{
		if (!spt_meshRenderer.signal.Works)
			return;
		InitConcommandBase(y_spt_draw_frustum);
		InitConcommandBase(y_spt_draw_frustum_far_plane);
		spt_meshRenderer.signal.Connect(this, &FrustumFeature::OnMeshRenderSignal);
	}

	void OnMeshRenderSignal(MeshRendererDelegate& mr)
	{
		if (!y_spt_draw_frustum.GetBool())
			return;

		if (!spt_entprops.GetPlayer(false))
			return;

		DrawFrustum(mr, true, MeshColor::Outline({255, 255, 0, 10}));
		DrawFrustum(mr, false, MeshColor::Face({255, 255, 0, 10}));
	}

	void DrawFrustum(MeshRendererDelegate& mr, bool reverse, MeshColor mc)
	{
		mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
		                [mc](MeshBuilderDelegate& mb)
		                {
			                VPlane planes[6];
			                memcpy(planes, spt_overlay.viewRender->GetFrustum(), sizeof(VPlane) * 6);
			                float maxDist = -1;
			                int maxDistIdx = -1;
			                for (int i = 0; i < 6; i++)
			                {
				                planes[i] = planes[i].Flip();
				                float tmp = abs(planes[i].DistTo(spt_overlay.mainView->origin));
				                if (tmp > maxDist)
				                {
					                maxDist = tmp;
					                maxDistIdx = i;
				                }
			                }
			                planes[maxDistIdx].m_Dist +=
			                    planes[maxDistIdx].DistTo(spt_overlay.mainView->origin)
			                    + y_spt_draw_frustum_far_plane.GetFloat();

			                CPolyhedron* poly = GeneratePolyhedronFromPlanes((float*)planes, 6, 0.1f, true);
			                if (poly)
			                {
				                mb.AddCPolyhedron(poly, mc);
				                poly->Release();
			                }
		                },
		                {ZTEST_FACES | ZTEST_LINES, reverse ? CullType::Reverse : CullType::Default}),
		            [](const CallbackInfoIn& infoIn, CallbackInfoOut& infoOut)
		            {
			            AngleIMatrix(spt_overlay.mainView->angles,
			                         spt_overlay.mainView->origin,
			                         infoOut.mat);
			            matrix3x4_t clientEyeMat;
			            Vector clientEyePos = spt_playerio.m_vecAbsOrigin.GetValue()
			                                  + spt_playerio.m_vecViewOffset.GetValue();
			            QAngle clientEyeAng;
			            EngineGetViewAngles((float*)&clientEyeAng);
			            AngleMatrix(clientEyeAng, clientEyePos, clientEyeMat);
			            MatrixMultiply(clientEyeMat, infoOut.mat, infoOut.mat);
		            });
	}
};

static FrustumFeature spt_frustumFeature;

#endif
