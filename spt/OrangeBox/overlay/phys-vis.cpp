#include "stdafx.h"

#include "phys-vis.h"
#include "portal_camera.hpp"
#include "..\modules.hpp"
#include "..\..\utils\ent_utils.hpp"
#include "..\..\utils\property_getter.hpp"

const matrix3x4_t matrix3x4_identity(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0);
const matrix3x4_t* curPhysCollideMat = nullptr;

/*
* This is a vphysicsDLL.CPhysicsCollision__CreateDebugMesh_Func, it mostly prevents z-fighting by pushing the faces
* away from the surface that they're on; the push amount depends on the distance from the mesh to the camera.
*/
int __fastcall PushFacesTowardsNormals(const IPhysicsCollision* thisptr,
                                       int dummy,
                                       const CPhysCollide* pCollisionModel,
                                       Vector** outVerts)
{
	int vertCount = vphysicsDLL.ORIG_CPhysicsCollision__CreateDebugMesh(thisptr, dummy, pCollisionModel, outVerts);
	Vector* v = *outVerts;
	const Vector camPos = clientDLL.GetCameraOrigin();
	// determine the furthest vert from the camera
	float maxDistSqr = 0;
	for (int i = 0; i < vertCount; i++)
	{
		float distSqr;
		if (curPhysCollideMat)
		{
			// outVerts doesn't have the transform applied yet
			Vector out;
			VectorTransform(v[i].Base(), *curPhysCollideMat, out.Base());
			distSqr = camPos.DistToSqr(out);
		}
		else
		{
			distSqr = camPos.DistToSqr(v[i]);
		}
		maxDistSqr = MAX(maxDistSqr, distSqr);
	}
	// expand the debug mesh by some epsilon - we still want the mesh to look good up close
	float normEps = MAX(pow(maxDistSqr, 0.6f) / 50000, 0.0001f);
	for (int i = 0; i < vertCount; i += 3)
	{
		Vector norm = (v[i] - v[i + 1]).Cross(v[i + 2] - v[i]);
		norm.NormalizeInPlace();
		norm *= normEps;
		v[i] += norm;
		v[i + 1] += norm;
		v[i + 2] += norm;
	}
	return vertCount;
}

// pass in null for the matrix to use the identity matrix
void DrawCPhysCollide(const CPhysCollide* pCollide,
                      const color32& c,
                      const matrix3x4_t* mat,
                      bool limitZFighting,
                      bool drawFaces,
                      bool drawWireframe)
{
	if (!pCollide)
		return;
	const char* faceMatName = "debug/debugtranslucentvertexcolor";
	IMaterial* faceMat = GetMaterialSystem()->FindMaterial(faceMatName, TEXTURE_GROUP_OTHER);
	if (limitZFighting)
	{
		// DebugDrawPhysCollide will call CreateDebugMesh, edit the mesh there
		vphysicsDLL.CPhysicsCollision__CreateDebugMesh_Func = PushFacesTowardsNormals;
		// if the matrix is null we can save some frames by not applying any transform during the push
		curPhysCollideMat = mat;
	}
	// the drawPhysCollide function however applies the matrix no matter what
	if (!mat)
		mat = &matrix3x4_identity;
	if (drawFaces)
		engineDLL.ORIG_DebugDrawPhysCollide(pCollide, faceMat, *mat, c, false);
	if (drawWireframe)
	{
		const color32& wireColor = drawFaces ? color32{0, 0, 0, 250} : c;
		engineDLL.ORIG_DebugDrawPhysCollide(pCollide, nullptr, *mat, wireColor, false);
	}
	vphysicsDLL.CPhysicsCollision__CreateDebugMesh_Func = nullptr;
	return;
}

void DrawCPhysicsObject(const void* pPhysicsObject,
                        const color32& c,
                        bool limitZFighting,
                        bool drawFaces,
                        bool drawWireframe)
{
	if (!pPhysicsObject)
		return;
	Vector pos;
	QAngle ang;
	vphysicsDLL.ORIG_CPhysicsObject__GetPosition(pPhysicsObject, 0, &pos, &ang);
	matrix3x4_t mat;
	AngleMatrix(ang, pos, mat);
	DrawCPhysCollide(*((CPhysCollide**)pPhysicsObject + 3), c, &mat, limitZFighting, drawFaces, drawWireframe);
}

void DrawCBaseEntity(const CBaseEntity* pEnt, const color32& c, bool limitZFighting, bool drawFaces, bool drawWireframe)
{
	if (!pEnt)
		return;
	DrawCPhysicsObject(*((void**)pEnt + 106), c, limitZFighting, drawFaces, drawWireframe);
}

ConVar y_spt_draw_portal_env_wireframe("y_spt_draw_portal_env_wireframe",
                                       "1",
                                       FCVAR_CHEAT | FCVAR_DONTRECORD,
                                       "draw the wireframe for static geometry");

ConVar y_spt_draw_portal_env_remote("y_spt_draw_portal_env_remote",
                                    "0",
                                    FCVAR_CHEAT | FCVAR_DONTRECORD,
                                    "draw geometry from the other portal");

ConVar y_spt_draw_portal_env_ents("y_spt_draw_portal_env_ents",
                                  "0",
                                  FCVAR_CHEAT | FCVAR_DONTRECORD,
                                  "draw entities owned by the portal and shadow clones from the other portal");

ConVar y_spt_draw_portal_env_type(
    "y_spt_draw_portal_env_type",
    "auto",
    FCVAR_CHEAT | FCVAR_DONTRECORD,
    "collide|auto|blue|orange|<index>; draw world collision and static props in a portal environment\n"
    "   - collide: draw what the player has collision with\n"
    "   - auto: prioritize what the player has collision with, otherwise use the last drawn portal\n"
    "   - blue/orange: look for a specific portal color\n"
    "   - index: specific the entity index of the portal");

ConVar y_spt_draw_portal_env("y_spt_draw_portal_env",
                             "0",
                             FCVAR_CHEAT | FCVAR_DONTRECORD,
                             "draw the geometry in a portal's physics environment");

void DrawPortalEnv(CBaseEntity* portal)
{
	bool wire = y_spt_draw_portal_env_wireframe.GetBool();
	uint32_t* simulator = (uint32_t*)portal + 327;

	// portal hole - not directly used for collision (green wireframe)
	DrawCPhysCollide(*(CPhysCollide**)(simulator + 70), color32{20, 255, 20, 255}, nullptr, false, false, true);
	// world brushes - wall geo in front of portal (red)
	DrawCPhysCollide(*(CPhysCollide**)(simulator + 76), color32{255, 20, 20, 70}, nullptr, true, true, wire);
	// local wall tube - edge of portal (green)
	DrawCPhysCollide(*(CPhysCollide**)(simulator + 94), color32{0, 255, 0, 200}, nullptr, false, true, wire);
	// local wall brushes - wall geo behind the portal (blue)
	DrawCPhysCollide(*(CPhysCollide**)(simulator + 101), color32{40, 40, 255, 60}, nullptr, true, true, wire);
	// static props (piss colored)
	const CUtlVector<char[28]>& clippedStaticProps = *(CUtlVector<char[28]>*)(simulator + 83);
	for (int i = 0; i < clippedStaticProps.Count(); i++)
	{
		const color32 c = color32{255, 255, 40, 50};
		DrawCPhysCollide(*((CPhysCollide**)clippedStaticProps[i] + 2), c, nullptr, true, true, wire);
	}
	if (y_spt_draw_portal_env_remote.GetBool())
	{
		// remote brushes (light pink); z-fighting adjustment is set since this frequently overlaps with local world geo
		DrawCPhysicsObject(*(void**)(simulator + 103), color32{255, 150, 150, 15}, true, true, wire);
		// remote static props (light yellow)
		const CUtlVector<void*>& remoteStaticProps = *(CUtlVector<void*>*)(simulator + 104);
		for (int i = 0; i < remoteStaticProps.Count(); i++)
			DrawCPhysicsObject(remoteStaticProps[i], color32{255, 255, 150, 15}, false, true, wire);
	}
	if (y_spt_draw_portal_env_ents.GetBool())
	{
		// owned entities (pink wireframe)
		const CUtlVector<CBaseEntity*>& ownedEnts = *(CUtlVector<CBaseEntity*>*)(simulator + 2171);
		for (int i = 0; i < ownedEnts.Count(); i++)
			DrawCBaseEntity(ownedEnts[i], color32{255, 100, 255, 255}, false, false, true);
		// shadow clones (brown); the above list also contains shadow clones so this will just draw over that
		const CUtlVector<CBaseEntity*>& shadowClones = *(CUtlVector<CBaseEntity*>*)(simulator + 2166);
		for (int i = 0; i < shadowClones.Count(); i++)
			DrawCBaseEntity(shadowClones[i], color32{120, 80, 30, 255}, false, false, true);
	}
}

static int lastPortalIndex = -1;

void DrawSgCollision()
{
	if (!vphysicsDLL.ORIG_CPhysicsCollision__CreateDebugMesh || !vphysicsDLL.ORIG_CPhysicsObject__GetPosition
	    || !engineDLL.ORIG_DebugDrawPhysCollide || !y_spt_draw_portal_env.GetBool()
	    || !GetEngine()->PEntityOfEntIndex(0) || !DoesGameLookLikePortal())
	{
		return;
	}

	const char* typeStr = y_spt_draw_portal_env_type.GetString();

	if (!strlen(typeStr))
		return;

	bool want_blue = !strcmp(typeStr, "blue");
	bool want_orange = !strcmp(typeStr, "orange");
	bool want_auto = !strcmp(typeStr, "auto");
	bool want_collide = !strcmp(typeStr, "collide");

findPortal:
	int portalIdx = -1;
	if (want_auto || want_collide)
	{
		auto pEnv = GetEnvironmentPortal();
		if (pEnv)
		{
			portalIdx = pEnv->entindex();
			if (want_collide)
			{
				// check if the portal is open
				int handle = utils::GetProperty<int>(portalIdx - 1, "m_hLinkedPortal");
				int index = (handle & (MAX_EDICTS - 1));
				if (index >= MAX_EDICTS - 1)
					return;
			}
		}
		else if (want_collide)
		{
			return; // not in bubble
		}
		else if (lastPortalIndex == -1)
		{
		lookForAnyPortal:
			want_blue = want_orange = true;
			want_auto = false;
			goto findPortal;
		}
		else
		{
			auto pEnt = utils::GetClientEntity(lastPortalIndex - 1);
			if (invalidPortal(pEnt))
			{
				lastPortalIndex = -1;
				goto lookForAnyPortal;
			}
			portalIdx = lastPortalIndex;
		}
	}
	else if (want_blue || want_orange)
	{
		int lastViableIndex = -1;
		for (int i = 1; i < MAX_EDICTS; ++i)
		{
			IClientEntity* ent = utils::GetClientEntity(i - 1);
			if (!invalidPortal(ent))
			{
				const char* modelName = utils::GetModelName(ent);
				bool is_orange = strstr(modelName, "portal2");
				if ((want_orange && is_orange) || (want_blue && !is_orange))
				{
					// prioritize an open portal
					int handle = utils::GetProperty<int>(ent->entindex() - 1, "m_hLinkedPortal");
					int index = (handle & (MAX_EDICTS - 1));
					if (index < MAX_EDICTS - 1)
					{
						portalIdx = i;
						break;
					}
					else
					{
						lastViableIndex = i;
					}
				}
			}
		}
		if (portalIdx == -1)
		{
			if (lastViableIndex == -1)
				return; // no portals of the given color
			portalIdx = lastViableIndex;
		}
	}
	else
	{
		portalIdx = atoi(typeStr);
		if (invalidPortal(utils::GetClientEntity(portalIdx - 1)))
			return; // given portal index isn't valid
	}
	lastPortalIndex = portalIdx;

	auto pEnt = GetEngine()->PEntityOfEntIndex(portalIdx);
	if (!pEnt)
		return;

	auto portal = pEnt->GetIServerEntity();
	if (!portal)
		return;

	DrawPortalEnv(portal->GetBaseEntity());
}
