#include "stdafx.h"

#include "sg-collision.h"

// I am the cookie monster, give me all of the includes!

#include <chrono>
#include <unordered_set>
#include "..\modules.hpp"
#include "..\spt-serverplugin.hpp"

#include "..\custom_interfaces.hpp"
#include "eiface.h"
#include "engine\iserverplugin.h"
#include "engine\ivdebugoverlay.h"
#include "icvar.h"
#include "vgui\ischeme.h"
#include "vguimatsurface\imatsystemsurface.h"

#include "..\..\sptlib-wrapper.hpp"
#include "..\..\utils\ent_utils.hpp"
#include "..\..\utils\math.hpp"
#include "..\..\utils\string_parsing.hpp"
#include "..\custom_interfaces.hpp"
#include "..\cvars.hpp"
#include "..\modules.hpp"
#include "..\scripts\srctas_reader.hpp"
#include "..\scripts\tests\test.hpp"
#include "..\..\ipc\ipc-spt.hpp"
#include "vstdlib\random.h"

#include "cdll_int.h"
#include "eiface.h"
#include "engine\iserverplugin.h"
#include "icliententitylist.h"
#include "tier2\tier2.h"
#include "tier3\tier3.h"
#include "vgui\iinput.h"
#include "vgui\isystem.h"
#include "vgui\ivgui.h"

#include "portal_camera.hpp"
#include "..\..\utils\property_getter.hpp"

// these structs are here in case I need to use them later, but I don't think I'll need them for anything
/*struct Polyhedron_IndexedLine_t
{
	unsigned short iPointIndices[2];
};

struct Polyhedron_IndexedLineReference_t
{
	unsigned short iLineIndex;
	unsigned char iEndPointIndex;
};

struct Polyhedron_IndexedPolygon_t
{
	unsigned short iFirstIndex;
	unsigned short iIndexCount;
	Vector polyNormal;
};

class CPolyhedron
{
public:
	Vector* pVertices;
	Polyhedron_IndexedLine_t* pLines;
	Polyhedron_IndexedLineReference_t* pIndices;
	Polyhedron_IndexedPolygon_t* pPolygons;

	unsigned short iVertexCount;
	unsigned short iLineCount;
	unsigned short iIndexCount;
	unsigned short iPolygonCount;
};*/

// void drawPolyhedrons(CUtlVector<CPolyhedron*> polyhedrons, int r, int g, int b) {}

ConVar y_spt_draw_collision_wireframe("y_spt_draw_collision_wireframe", "1");

void DrawCPhysCollide(const CPhysCollide* pCollide, const color32& c, const matrix3x4_t& mat, bool drawWireframe)
{
	if (!pCollide)
		return;
	IMaterial* pMaterial =
	    GetMaterialSystem()->FindMaterial("debug/debugtranslucentvertexcolor", TEXTURE_GROUP_OTHER);
	// DebugDrawPhysCollide uses CreateDebugMesh from vphysics, edit the mesh there
	vphysicsDLL.adjustDebugMesh = true;
	engineDLL.ORIG_DebugDrawPhysCollide(pCollide, pMaterial, mat, c, false);
	if (drawWireframe)
		engineDLL.ORIG_DebugDrawPhysCollide(pCollide, nullptr, mat, color32{0, 0, 0, 250}, false);
	vphysicsDLL.adjustDebugMesh = false;
	return;
}

void DrawCPhysicsObject(const void* pPhysicsObject, const color32& c, bool drawWireframe)
{
	if (!pPhysicsObject)
		return;
	Vector pos;
	QAngle ang;
	vphysicsDLL.ORIG_CPhysicsObject__GetPosition(pPhysicsObject, 0, &pos, &ang);
	matrix3x4_t mat;
	AngleMatrix(ang, pos, mat);
	DrawCPhysCollide(*((CPhysCollide**)pPhysicsObject + 3), c, mat, drawWireframe);
}

void DrawCollisionForPortal(CBaseEntity* portal)
{
	bool wire = y_spt_draw_collision_wireframe.GetBool();
	matrix3x4_t mat;
	AngleMatrix(vec3_angle, vec3_origin, mat);
	uint32_t* simulator = (uint32_t*)portal + 327;

	// world brushes - wall geo in front of portal (red)
	DrawCPhysCollide(*(CPhysCollide**)(simulator + 76), color32{255, 20, 20, 70}, mat, wire);
	// local wall tube - the portal hole itself (probably not actually used for collision) (green)
	DrawCPhysCollide(*(CPhysCollide**)(simulator + 94), color32{0, 255, 0, 200}, mat, wire);
	// local wall brushes - wall geo behind the portal (blue)
	DrawCPhysCollide(*(CPhysCollide**)(simulator + 101), color32{40, 40, 255, 60}, mat, wire);

	// static props (piss colored)
	const CUtlVector<char[28]>& clippedStaticProps = *(CUtlVector<char[28]>*)(simulator + 83);
	for (int i = 0; i < clippedStaticProps.Count(); i++)
		DrawCPhysCollide(*((CPhysCollide**)clippedStaticProps[i] + 2), color32{255, 255, 40, 50}, mat, wire);

	uint32_t* linkedSim = *(uint32_t**)(simulator + 1);
	if (linkedSim)
	{
		AssertMsg(*(uint32_t**)(linkedSim + 1), "pointer to linked simulator makes no sense");
		// remote brushes (white)
		DrawCPhysicsObject(*(void**)(simulator + 103), color32{200, 200, 200, 20}, wire);
		// remote static props (light-yellow)
		const CUtlVector<void*>& remoteStaticProps = *(CUtlVector<void*>*)(simulator + 104);
		for (int i = 0; i < remoteStaticProps.Count(); i++)
			DrawCPhysicsObject(remoteStaticProps[i], color32{255, 255, 100, 20}, wire);
	}

	// TODO:
	// look at the dynamic stuff
	// look at stuff that only has IPhysicsObject's e.g. Simulation.Static.Wall.RemoteTransformedToLocal.Brushes
	// look at the whole physics environment?
}

static int lastPortalIndex = -1;

static ConVar y_spt_draw_portal_collision(
    "y_spt_draw_portal_collision",
    "",
    FCVAR_DONTRECORD,
    "collide|auto|blue|orange|<index>\n"
    "   - collide: draw what the player has collision with\n"
    "   - auto: prioritize what the player has collision with, otherwise use the last drawn portal\n"
    "   - blue/orange: look for a specific portal color\n"
    "   - index: specific the entity index of the portal");

void DrawSgCollision()
{
	if (!GetEngine()->PEntityOfEntIndex(0))
		return; // playing a demo (I think)

	const char* str = y_spt_draw_portal_collision.GetString();

	if (!strlen(str))
		return;

	bool want_blue = !strcmp(str, "blue");
	bool want_orange = !strcmp(str, "orange");
	bool want_auto = !strcmp(str, "auto");
	bool want_collide = !strcmp(str, "collide");

	if (!want_auto && !want_collide && !want_blue && !want_orange && !atoi(str))
		return;

findPortal:
	int portalIndex = -1;
	if (want_auto || want_collide)
	{
		auto pEnv = GetEnvironmentPortal();
		if (pEnv)
		{
			portalIndex = pEnv->entindex();
			if (want_collide)
			{
				// check if the portal is open
				int handle = utils::GetProperty<int>(portalIndex - 1, "m_hLinkedPortal");
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
			portalIndex = lastPortalIndex;
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
						portalIndex = i;
						break;
					}
					else
					{
						lastViableIndex = i;
					}
				}
			}
		}
		if (portalIndex == -1)
		{
			if (lastViableIndex == -1)
				return; // no portals of the given color
			portalIndex = lastViableIndex;
		}
	}
	else
	{
		portalIndex = atoi(str);
		if (portalIndex < 1 || portalIndex >= MAX_EDICTS || invalidPortal(utils::GetClientEntity(portalIndex)))
			return; // given portal index isn't valid
	}
	lastPortalIndex = portalIndex;

	auto pEnt = GetEngine()->PEntityOfEntIndex(portalIndex);
	if (!pEnt)
		return;

	auto portal = pEnt->GetIServerEntity();
	if (!portal)
		return;

	DrawCollisionForPortal(portal->GetBaseEntity());
}
