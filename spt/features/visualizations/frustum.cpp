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

class FrustumFeature : public FeatureWrapper<FrustumFeature>
{
	void LoadFeature() override
	{
		if (!spt_meshRenderer.signal.Works)
			return;
		InitConcommandBase(y_spt_draw_frustum);
		spt_meshRenderer.signal.Connect(this, &FrustumFeature::OnMeshRenderSignal);
	}

	void OnMeshRenderSignal(MeshRendererDelegate& mr)
	{
		if (!y_spt_draw_frustum.GetBool())
			return;

		if (!spt_entprops.GetPlayer(false))
			return;

		mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
		                [](MeshBuilderDelegate& mb)
		                {
			                VPlane planes[6];
			                memcpy(planes, spt_overlay.viewRender->GetFrustum(), sizeof(VPlane) * 6);
			                for (int i = 0; i < 6; i++)
				                planes[i] = planes[i].Flip();
			                CPolyhedron* poly = GeneratePolyhedronFromPlanes((float*)planes, 6, 0.1f, true);
			                mb.AddCPolyhedron(poly, MeshColor::Outline({255, 255, 0, 10}));
			                poly->Release();
		                }),
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
