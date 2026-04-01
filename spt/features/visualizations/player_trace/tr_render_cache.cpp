#include "stdafx.hpp"

#include "tr_render_cache.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "spt/features/visualizations/renderer/mesh_renderer.hpp"
#include "spt/features/ent_props.hpp"
#include "spt/utils/interfaces.hpp"
#include "spt/utils/map_utils.hpp"
#include "spt/utils/math.hpp"

#define PORTAL_HALF_WIDTH 32.0f
#define PORTAL_HALF_HEIGHT 54.0f

using namespace player_trace;

template<typename T>
static bool CheckUpdateCache(T& cached, const T& newVal)
{
	bool dirty = memcmp(&cached, &newVal, sizeof(T));
	if (dirty)
		memcpy(&cached, &newVal, sizeof(T));
	return dirty;
}

void TrRenderingCache::RebuildPlayerHullMeshes()
{
	auto& tr = TrReadContextScope::Current();

	bool cfgDirty = CheckUpdateCache(cfg.playerHull, pNewCfg->playerHull);
	auto& hullCfg = pNewCfg->playerHull;

	// qphys

	static constexpr float qphysHullReduction = 0.7f;

	struct
	{
		StaticMesh& mesh;
		TrIdx<TrAbsBox> hullIdx;
	} qPhysData[] = {
	    {meshes.qPhysStand, tr.playerStandBboxIdx},
	    {meshes.qPhysDuck, tr.playerDuckBboxIdx},
	};

	for (auto& [mesh, hullIdx] : qPhysData)
	{
		if (!cfgDirty && mesh.Valid())
			continue;

		mesh = spt_meshBuilder.CreateStaticMesh(
		    [hullIdx, &hullCfg](MeshBuilderDelegate& mb)
		    {
			    const Vector& mins = **hullIdx->minsIdx;
			    const Vector& maxs = **hullIdx->maxsIdx;
			    mb.AddBox(vec3_origin, mins, maxs, vec3_angle, hullCfg.colors.qPhys);
			    mb.AddBox(vec3_origin,
			              Vector{mins.x * qphysHullReduction, mins.y * qphysHullReduction, mins.z},
			              Vector{maxs.x * qphysHullReduction, maxs.y * qphysHullReduction, maxs.z},
			              vec3_angle,
			              hullCfg.colors.qPhysReduction);
			    if (hullCfg.drawOriginCube)
			    {
				    mb.AddBox(vec3_origin,
				              Vector{-hullCfg.originCubeSize},
				              Vector{hullCfg.originCubeSize},
				              vec3_angle,
				              hullCfg.colors.qPhysOrigin);
			    }
			    if (hullCfg.drawQPhysCenter)
			    {
				    // `ent_bbox player` usually draws a cross at the vphys center, but qphys is prolly more relevant (for portals)
				    mb.AddCross((mins + maxs) * .5f,
				                hullCfg.qPhysCenterCrossRadius,
				                hullCfg.colors.qPhys.lineColor);
			    }
			    // add a small almost invisible shell around the qphhys box to try to prevent common z-fighting cases
			    mb.AddBox(vec3_origin,
			              mins - Vector{0.001f},
			              maxs + Vector{0.001f},
			              vec3_angle,
			              ShapeColor{C_WIRE(0, 0, 0, 1)});
		    });
	}

	// vphys

	struct
	{
		StaticMesh& mesh;
		TrIdx<TrAbsBox> hullIdx;
	} vPhysData[] = {
	    {meshes.vPhysStand, tr.playerStandBboxIdx},
	    {meshes.vPhysDuck, tr.playerDuckBboxIdx},
	};

	for (auto& [mesh, hullIdx] : vPhysData)
	{
		if (!cfgDirty && mesh.Valid())
			continue;
		mesh = spt_meshBuilder.CreateStaticMesh(
		    [hullIdx, &hullCfg](MeshBuilderDelegate& mb)
		    {
			    const Vector& mins = **hullIdx->minsIdx;
			    const Vector& maxs = **hullIdx->maxsIdx;
			    mb.AddBox(vec3_origin, mins, maxs, vec3_angle, hullCfg.colors.vPhys);
		    });
	}
}

void TrRenderingCache::RebuildEyeMeshes(float fov)
{
	if (CheckUpdateCache(cfg.playerEye, pNewCfg->playerEye) || meshes.lastEyeMeshFov != fov)
		StaticMesh::DestroyAllV(meshes.eyes, meshes.sgEyes);

	auto& eyeCfg = pNewCfg->playerEye;

	using EyeType = TrRenderStyleConfig::PlayerEye::EyeType;
	meshes.lastEyeMeshFov = fov;

	auto buildFrustumFunc = [fov, &eyeCfg](MeshBuilderDelegate& mb, EyeType eyeType)
	{
		auto& frustumStyle = eyeCfg.frustum;
		ShapeColor color = frustumStyle.colors[eyeType];
		float scaledFov = utils::ScaleFOVByWidthRatio(fov, frustumStyle.aspect * 3.f / 4.f);
		float halfHorizontalAng = DEG2RAD(scaledFov * .5f);
		float halfVerticalAng = atanf(tanf(halfHorizontalAng) / frustumStyle.aspect);

		float hc, hs, vc, vs;
		SinCos(halfHorizontalAng, &hs, &hc);
		SinCos(halfVerticalAng, &vs, &vc);
		VPlane planes[] = {
		    {{-hs, hc, 0}, 0},
		    {{-hs, -hc, 0}, 0},
		    {{-vs, 0, vc}, 0},
		    {{-vs, 0, -vc}, 0},
		    {{1, 0, 0}, frustumStyle.zFar},
		};
		CPolyhedron* poly = GeneratePolyhedronFromPlanes((float*)planes, ARRAYSIZE(planes), 0.001f, true);
		mb.AddCPolyhedron(poly, color);
		if (poly)
			poly->Release();

		color.wd = WD_BOTH;
		Vector p1 = Vector{
		    frustumStyle.zFar,
		    frustumStyle.zFar * hs / hc,
		    frustumStyle.zFar * vs / vc + frustumStyle.hatFloat,
		};
		Vector p2{p1.x, -p1.y, p1.z};
		Vector p3 = (p1 + p2) * .5f + Vector{0, 0, frustumStyle.hatHeight};
		mb.AddTri(p1, p2, p3, color);
	};

	auto buildBoxAndLineFunc = [&eyeCfg](MeshBuilderDelegate& mb, EyeType eyeType)
	{
		auto& style = eyeCfg.boxAndLine;
		mb.AddBox(vec3_origin,
		          Vector{-style.boxRadius},
		          Vector{style.boxRadius},
		          vec3_angle,
		          style.colors[eyeType]);
		mb.AddLine(vec3_origin, Vector{style.lineLength, 0, 0}, style.colors[eyeType].faceColor);
	};

	struct
	{
		StaticMesh& mesh;
		EyeType eyeType;
	} eyeMeshData[] = {
	    {meshes.eyes, EyeType::TR_EYE_ACTUAL},
	    {meshes.sgEyes, EyeType::TR_EYE_SAVEGLITCH},
	};

	for (auto& [mesh, eyeType] : eyeMeshData)
	{
		if (mesh.Valid())
			continue;
		mesh = spt_meshBuilder.CreateStaticMesh(
		    [&](MeshBuilderDelegate& mb)
		    {
			    if (eyeCfg.style == TR_PCDT_FRUSTUM)
				    buildFrustumFunc(mb, eyeType);
			    else
				    buildBoxAndLineFunc(mb, eyeType);
		    });
	}
}

void TrRenderingCache::RebuildPlayerPathMeshes()
{
	auto& tr = TrReadContextScope::Current();

	meshes.playerPath.dynamicMeshes.clear();

	bool clearStatics = CheckUpdateCache(cfg.playerPath, pNewCfg->playerPath);
	if (!clearStatics)
		clearStatics = !StaticMesh::AllValid(meshes.playerPath.staticMeshes);
	if (!clearStatics)
	{
		clearStatics = (*pNewEnableCfg)[TR_RENDER_ENABLE_PLAYER_PATH_CONES]
		                   != enableCfg[TR_RENDER_ENABLE_PLAYER_PATH_CONES]
		               || (*pNewEnableCfg)[TR_RENDER_ENABLE_PLAYER_PATH_ENDPOINTS]
		                      != enableCfg[TR_RENDER_ENABLE_PLAYER_PATH_ENDPOINTS];
	}

	if (clearStatics)
		meshes.playerPath.staticMeshes.clear();

	if (meshes.playerPath.staticMeshes.empty())
		meshes.playerPath.staticMeshesBuiltUpToTick = 0;

	if (meshes.playerPath.staticMeshesBuiltUpToTick + 1 >= tr.numRecordedTicks)
		return;

	auto& pathCfg = pNewCfg->playerPath;

	/*
	* TODO: right now, it's possible to overdraw if e.g. the line gets drawn but the cone doesn't -
	* the line will get added in the next mesh as well. To properly fix this we need support for
	* transactions directly in the MeshBuilderDelegate. This is useful for other meshes as well.
	*/

	/*
	* The whole player path gets built as one mesh (if possible). As we iterate, the landmark
	* transforms are added directly to the player path. This means that all of the coordinates in
	* the mesh(es) are relative to the first map. If we're loaded in another map, we have to
	* transform the mesh(es).
	*/

	tr_tick firstTickToDraw = meshes.playerPath.staticMeshesBuiltUpToTick;

	auto pdIdx = tr.GetAtTick<TrPlayerData>(firstTickToDraw);
	auto segmentIdx = tr.GetAtTick<TrSegmentStart>(firstTickToDraw);
	auto mapTransitionIdx = tr.GetAtTick<TrMapTransition>(firstTickToDraw);

	Vector lastImplicitVel{1.f, 0.f, 0.f};
	Vector prevImplicitVel = lastImplicitVel;
	Vector landmarkOff =
	    mapTransitionIdx.IsValid() ? **mapTransitionIdx->toMapIdx->landmarkDeltaToFirstMapIdx : vec3_origin;
	int ticksWithoutCone = 0;
	TrSegmentReason deferredSegmentReason = TR_SR_NONE;

	if (!pdIdx.IsValid())
		return;

	auto createFunc = [&](MeshBuilderDelegate& mb, tr_tick tick)
	{
		auto incrementIdxToCurTick = [tick]<typename T>(TrIdx<T>& idx)
		{
			if (!(idx + 1).IsValid() || tick < (idx + 1)->tick)
				return false;
			++idx;
			return true;
		};

		Vector p1 = **pdIdx->qPosIdx + landmarkOff;

		incrementIdxToCurTick(pdIdx);
		if (incrementIdxToCurTick(mapTransitionIdx))
			landmarkOff = **mapTransitionIdx->toMapIdx->landmarkDeltaToFirstMapIdx;

		Vector p2 = **pdIdx->qPosIdx + landmarkOff;

		bool drawCones = (*pNewEnableCfg)[TR_RENDER_ENABLE_PLAYER_PATH_CONES];
		bool drawPath = true;
		TrSegmentReason segmentReason = TR_SR_NONE;

		if (p1 == p2)
		{
			drawPath = false;
			drawCones = false;
		}
		else
		{
			prevImplicitVel = lastImplicitVel;
			lastImplicitVel = p2 - p1;
		}

		bool ptsValid = p1.IsValid() && p2.IsValid();
		if (!ptsValid)
		{
			drawPath = false;
			drawCones = false;
			segmentReason = TR_SR_NONE;
		}

		if (incrementIdxToCurTick(segmentIdx))
		{
			drawPath = false;
			segmentReason = segmentIdx->reason;
		}
		else if (ptsValid && p2.DistToSqr(p1) > pathCfg.segments.maxDistBeforeImplicitBreakSqr)
		{
			drawPath = false;
			segmentReason = TR_SR_IMPLICIT;
		}
		else
		{
			segmentReason = TR_SR_NONE;
		}

		LineColor pathCol = pathCfg.segments.colors.grounded;
		if (drawPath)
		{
			if (!mb.AddLine(p1, p2, pathCol)) [[unlikely]]
				return false;
		}

		if ((*pNewEnableCfg)[TR_RENDER_ENABLE_PLAYER_PATH_ENDPOINTS])
			segmentReason = TR_SR_NONE;

		if (drawCones && segmentReason == TR_SR_NONE && ticksWithoutCone++ > pathCfg.cones.tickInterval)
		{
			ticksWithoutCone = 0;
			Vector implVelNorm = lastImplicitVel;
			VectorNormalize(implVelNorm);
			QAngle ang;
			VectorAngles(implVelNorm, ang);
			ShapeColor coneCol{
			    pathCol.lineColor,
			    pathCol.lineColor,
			    pathCol.zTestLines,
			    pathCol.zTestLines,
			};
			coneCol.faceColor.a *= pathCfg.cones.opacity;
			if (!mb.AddCone(p2 - implVelNorm * pathCfg.cones.length,
			                ang,
			                pathCfg.cones.length,
			                pathCfg.cones.radius,
			                pathCfg.cones.nCirclePoints,
			                false,
			                coneCol))
			{
				return false;
			}
		}

		if (segmentReason != TR_SR_NONE || deferredSegmentReason != TR_SR_NONE)
		{
			bool firstCircle = segmentReason != TR_SR_NONE;

			TrSegmentReason curReason = firstCircle ? segmentReason : deferredSegmentReason;
			Vector vel = firstCircle ? prevImplicitVel : lastImplicitVel;

			if (firstCircle)
				deferredSegmentReason = segmentReason;
			else
				deferredSegmentReason = TR_SR_NONE;

			QAngle ang;
			VectorAngles(vel, ang);
			ShapeColor endpointColor = pathCfg.endpoints.colors[curReason];
			endpointColor.faceColor.a *= pathCfg.endpoints.opacity;
			if (!mb.AddCircle(p1,
			                  ang,
			                  pathCfg.endpoints.radius,
			                  pathCfg.endpoints.nCirclePoints,
			                  endpointColor))
			{
				return false;
			}
		}

		tick++;
		return true;
	};

	tr_tick maxTicksForDynamic = tr.IsRecording() ? pathCfg.segments.maxTicksToRenderAsDynamicMesh : 0;

	if (tr.numRecordedTicks - meshes.playerPath.staticMeshesBuiltUpToTick > maxTicksForDynamic)
	{
		spt_meshBuilder.CreateMultipleMeshes<StaticMesh>(std::back_inserter(meshes.playerPath.staticMeshes),
		                                                 meshes.playerPath.staticMeshesBuiltUpToTick,
		                                                 tr.numRecordedTicks,
		                                                 createFunc);

		meshes.playerPath.staticMeshesBuiltUpToTick = tr.numRecordedTicks - 1;
	}
	else
	{
		spt_meshBuilder.CreateMultipleMeshes<DynamicMesh>(std::back_inserter(meshes.playerPath.dynamicMeshes),
		                                                  meshes.playerPath.staticMeshesBuiltUpToTick,
		                                                  tr.numRecordedTicks,
		                                                  createFunc);
	}
}

void TrRenderingCache::RebuildPortalMeshes()
{
	bool cfgDirty = CheckUpdateCache(cfg.portals, pNewCfg->portals);
	auto& portalCfg = pNewCfg->portals;

	if (!cfgDirty)
	{
		if (StaticMesh::AllValidV(meshes.openBluePortal,
		                          meshes.openOrangePortal,
		                          meshes.closedBluePortal,
		                          meshes.closedOrangePortal))
		{
			return;
		}
	}

	auto buildFunc = [this, &portalCfg](bool open, bool orange)
	{
		return spt_meshBuilder.CreateStaticMesh(
		    [open, orange, &portalCfg](MeshBuilderDelegate& mb)
		    {
			    ShapeColor col = orange ? portalCfg.colors.orange : portalCfg.colors.blue;
			    if (!open)
				    col.faceColor.a /= 4;
			    Vector boxMaxs{
			        portalCfg.portalThickness * 0.5f,
			        PORTAL_HALF_WIDTH,
			        PORTAL_HALF_HEIGHT,
			    };
			    mb.AddBox(vec3_origin, -boxMaxs, boxMaxs, vec3_angle, col);

			    col.wd = WD_BOTH;
			    Vector p1{boxMaxs.x, PORTAL_HALF_WIDTH, PORTAL_HALF_HEIGHT + portalCfg.hatFloat};
			    Vector p2{p1.x, -p1.y, p1.z};
			    Vector p3 = (p1 + p2) * .5f + Vector{0, 0, portalCfg.hatHeight};
			    mb.AddTri(p1, p2, p3, col);

			    col.wd = WD_CW;
			    Vector arrowPos{boxMaxs.x, 0, 0};
			    mb.AddArrow3D(arrowPos, arrowPos + Vector{1, 0, 0}, portalCfg.arrowParams, col);
		    });
	};

	meshes.openBluePortal = buildFunc(true, false);
	meshes.openOrangePortal = buildFunc(true, true);
	meshes.closedBluePortal = buildFunc(false, false);
	meshes.closedOrangePortal = buildFunc(false, true);
}

void TrRenderingCache::RebuildPhysMeshes(tr_tick atTick)
{
	auto& tr = TrReadContextScope::Current();
	auto& entCache = tr.GetEntityCache();
	auto& entMap = entCache.GetEnts(atTick);

	bool cfgDirty = CheckUpdateCache(cfg.entPhys, pNewCfg->entPhys);
	auto& physCfg = pNewCfg->entPhys;
	auto& physMeshes = meshes.ents.physObjs;

	for (auto& [_, tracked] : physMeshes)
		tracked.isActive = false;

	for (auto& [entIdx, entTransIdx] : entMap)
	{
		const TrEnt& ent = **entIdx;

		bool isPortalCollisionEnt = !strcmp(*entIdx->classNameIdx, "portalsimulator_collisionentity");
		const ShapeColor& shapeCol = isPortalCollisionEnt ? physCfg.color : physCfg.portalCollisionEnts.color;

		for (auto physIdx : *ent.physSp)
		{
			auto [it, new_elem] = physMeshes.try_emplace(physIdx->meshIdx);
			it->second.isActive = true;
			if (!cfgDirty && it->second.mesh.Valid())
				continue;

			const TrPhysMesh& physMesh = **it->first;

			if (physMesh.ballRadius > 0)
			{
				it->second.mesh = spt_meshBuilder.CreateStaticMesh(
				    [&](MeshBuilderDelegate& mb)
				    {
					    mb.AddSphere(vec3_origin,
					                 physMesh.ballRadius,
					                 physCfg.nBallMeshSubdivisions,
					                 shapeCol);
				    });
			}
			else
			{
				it->second.mesh = spt_meshBuilder.CreateStaticMesh(
				    [&](MeshBuilderDelegate& mb)
				    {
					    auto sp = *physMesh.vertIdxSp;
					    for (size_t i = 0; i < sp.size(); i += 3)
						    mb.AddTri(**sp[i], **sp[i + 1], **sp[i + 2], shapeCol);
				    });
			}
		}
	}

	std::erase_if(physMeshes, [](const auto& entry) { return !entry.second.isActive; });
}

void TrRenderingCache::RenderPlayerPath(MeshRendererDelegate& mr, const Vector& landmarkDeltaToFirstMap)
{
	RebuildPlayerPathMeshes();

	RenderCallback cb = [landmarkDeltaToFirstMap](const CallbackInfoIn& infoIn, CallbackInfoOut& infoOut)
	{ PositionMatrix(landmarkDeltaToFirstMap, infoOut.mat); };

	for (auto& m : meshes.playerPath.staticMeshes)
		mr.DrawMesh(m, cb);
	for (auto& m : meshes.playerPath.dynamicMeshes)
		mr.DrawMesh(m, cb);
}

void TrRenderingCache::RenderPlayerHull(MeshRendererDelegate& mr,
                                        const Vector& landmarkDeltaToMapAtTick,
                                        tr_tick atTick)
{
	auto& tr = TrReadContextScope::Current();

	auto pdIdx = tr.GetAtTick<TrPlayerData>(atTick);
	if (!pdIdx.IsValid())
		return;

	RebuildPlayerHullMeshes();
	RebuildEyeMeshes(pdIdx->fov == 0 ? 90.f : pdIdx->fov);

	const TrPlayerData& pd = **pdIdx;
	TrTransform qPhysTransform{pd.qPosIdx, {}};

	struct
	{
		StaticMesh& mesh;
		const TrTransform& trans;
		bool bDraw = true;
	} hulls[] = {
	    {(pd.m_fFlags & FL_DUCKING) ? meshes.qPhysDuck : meshes.qPhysStand, qPhysTransform},
	    {meshes.eyes, **pd.transEyesIdx},
	    {meshes.sgEyes, **pd.transSgEyesIdx, pd.transEyesIdx != pd.transSgEyesIdx},
	    {(pd.m_fFlags & FL_DUCKING) ? meshes.vPhysDuck : meshes.vPhysStand, **pd.transVPhysIdx},
	};

	for (auto& [mesh, trans, bDraw] : hulls)
	{
		if (!bDraw || !mesh.Valid())
			continue;
		Vector pos = **trans.posIdx + landmarkDeltaToMapAtTick;
		QAngle ang = trans.angIdx.IsValid() ? **trans.angIdx : vec3_angle;
		if (!pos.IsValid() || !ang.IsValid())
			continue;
		mr.DrawMesh(mesh,
		            [pos, ang](const CallbackInfoIn& infoIn, CallbackInfoOut& infoOut)
		            { AngleMatrix(ang, pos, infoOut.mat); });
	}

	if ((*pNewEnableCfg)[TR_RENDER_ENABLE_PLAYER_CONTACT_POINTS])
	{
		for (auto contactPtIdx : *pd.contactPtsSp)
		{
			mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
			    [contactPtIdx, &pd, &landmarkDeltaToMapAtTick, this](MeshBuilderDelegate& mb)
			    {
				    auto& contactCfg = pNewCfg->contactPoints;
				    Vector maxs{1.f};
				    const Vector& pos = **contactPtIdx->posIdx + landmarkDeltaToMapAtTick;
				    mb.AddBox(pos, -maxs, maxs, vec3_angle, contactCfg.color);
				    // DebugDrawContactPoints does (pt - norm * len), not sure why it's not (pt + norm * len)
				    mb.AddLine(pos,
				               pos - **contactPtIdx->normIdx * contactCfg.normalLength,
				               contactCfg.color.lineColor);
			    }));
		}
	}
}

void TrRenderingCache::RenderPortals(MeshRendererDelegate& mr, const Vector& landmarkDeltaToMapAtTick, tr_tick atTick)
{
	auto& tr = TrReadContextScope::Current();
	RebuildPortalMeshes();

	auto snapIdx = tr.GetAtTick<TrPortalSnapshot>(atTick);
	if (!snapIdx.IsValid())
		return;

	for (auto idx : *snapIdx->portalsSp)
	{
		const TrPortal& portal = **idx;
		if (!portal.isActivated)
			continue; // not rendering these for now
		const StaticMesh& mesh = portal.isOpen
		                             ? (portal.isOrange ? meshes.openOrangePortal : meshes.openBluePortal)
		                             : (portal.isOrange ? meshes.closedOrangePortal : meshes.closedBluePortal);
		if (!mesh.Valid())
			continue;
		Vector pos = **portal.transIdx->posIdx + landmarkDeltaToMapAtTick;
		QAngle ang = **portal.transIdx->angIdx;
		mr.DrawMesh(mesh,
		            [pos, ang](const CallbackInfoIn& infoIn, CallbackInfoOut& infoOut)
		            { AngleMatrix(ang, pos, infoOut.mat); });
	}
}

void TrRenderingCache::RenderEntities(MeshRendererDelegate& mr, const Vector& landmarkDeltaToMapAtTick, tr_tick atTick)
{
	auto& tr = TrReadContextScope::Current();

	if ((*pNewEnableCfg)[TR_RENDER_ENABLE_ENT_COLLECT_AABB])
	{
		auto traceStateIdx = tr.GetAtTick<TrTraceState>(atTick);
		auto plDataIdx = tr.GetAtTick<TrPlayerData>(atTick);
		if (plDataIdx.IsValid() && plDataIdx->qPosIdx->IsValid() && traceStateIdx.IsValid())
		{
			mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
			    [&](MeshBuilderDelegate& mb)
			    {
				    mb.AddBox(**plDataIdx->qPosIdx + landmarkDeltaToMapAtTick,
				              **traceStateIdx->entCollectBboxAroundPlayerIdx->minsIdx,
				              **traceStateIdx->entCollectBboxAroundPlayerIdx->maxsIdx,
				              vec3_angle,
				              pNewCfg->entCollectAabb.color);
			    }));
		}
	}

	if ((*pNewEnableCfg)[TR_RENDER_ENABLE_ENT_OBB])
		RenderEntObbs(mr, landmarkDeltaToMapAtTick, atTick);

	if ((*pNewEnableCfg)[TR_RENDER_ENABLE_ENT_PHYS])
	{
		RebuildPhysMeshes(atTick);
		RenderEntPhysMeshes(mr, landmarkDeltaToMapAtTick, atTick);
	}
}

void TrRenderingCache::RenderEntObbs(MeshRendererDelegate& mr, const Vector& landmarkDeltaToMapAtTick, tr_tick atTick)
{
	auto& tr = TrReadContextScope::Current();
	auto& entCache = tr.GetEntityCache();
	auto& entMap = entCache.GetEnts(atTick);

	auto& obbCfg = pNewCfg->entObb;

	const char* allowedTriggers[] = {
	    "trigger_once",
	    "trigger_transition",
	    "trigger_changelevel",
	    "trigger_portal_cleanser",
	    "trigger_hurt",
	};
	uint32_t triggerFlags = FSOLID_NOT_SOLID | FSOLID_TRIGGER;

	// draw each OBB as a dynamic mesh, the mesh system will fuse them anyways
	for (auto [entIdx, transIdx] : entMap)
	{
		if (entIdx->m_nSolidType == SOLID_NONE)
			continue;

		const char* className = *entIdx->classNameIdx;
		bool isTrigger = false;
		if ((entIdx->m_usSolidFlags & triggerFlags) == triggerFlags)
		{
			isTrigger = std::any_of(allowedTriggers,
			                        allowedTriggers + ARRAYSIZE(allowedTriggers),
			                        [className](const char* allowedTriggerName)
			                        { return !strcmp(className, allowedTriggerName); });
			if (!isTrigger)
				continue; // don't draw other trigger types
		}
		if (isTrigger && !obbCfg.enableTriggers)
			continue;

		const TrAbsBox& obb = **transIdx->obbIdx;
		const TrTransform& trans = **transIdx->obbTransIdx;
		Vector mins = obb.minsIdx.GetOrDefault(Vector{NAN}), maxs = obb.maxsIdx.GetOrDefault(Vector{NAN});
		Vector pos;
		QAngle ang;
		trans.GetPosAng(pos, ang);

		if (!mins.IsValid() || !maxs.IsValid() || !pos.IsValid() || !ang.IsValid())
			continue;

		mr.DrawMesh(spt_meshBuilder.CreateDynamicMesh(
		    [&](MeshBuilderDelegate& mb)
		    {
			    // add in landmark delta manually so that meshes can be merged
			    Vector origin = pos + landmarkDeltaToMapAtTick;
			    // interfaces::debugOverlay->AddTextOverlay(origin, 0, "%s", className);
			    ShapeColor color = isTrigger ? obbCfg.colorTrigger : obbCfg.color;
			    if (mins != maxs)
				    mb.AddBox(origin, mins, maxs, ang, color);

			    // a position at exactly the origin *probably* means it's not relevant
			    if (obbCfg.enableCenterCross && !isTrigger && pos != vec3_origin)
				    mb.AddCross(origin, obbCfg.centerCrossRadius, color.lineColor);
			    if (entIdx == entCache.hoveredEnt)
			    {
				    Vector extra{1.f};
				    if (mins == maxs)
					    mins = -(maxs = Vector{obbCfg.centerCrossRadius});
				    mb.AddBox(origin, mins - extra, maxs + extra, ang, obbCfg.colorHovered);
			    }
		    }));
	}
}

void TrRenderingCache::RenderEntPhysMeshes(MeshRendererDelegate& mr,
                                           const Vector& landmarkDeltaToMapAtTick,
                                           tr_tick atTick)
{
	auto& tr = TrReadContextScope::Current();
	auto& entCache = tr.GetEntityCache();
	auto& entMap = entCache.GetEnts(atTick);

	for (auto [entIdx, transIdx] : entMap)
	{
		bool isPortalCollisionEnt = !strcmp(*entIdx->classNameIdx, "portalsimulator_collisionentity");
		if (isPortalCollisionEnt && !(*pNewEnableCfg)[TR_RENDER_ENABLE_PORTAL_COLLISION_ENTS])
			continue;

		auto physMeshTransIdxSp = *transIdx->physTransSp;
		auto physIdxsp = *entIdx->physSp;
		Assert(physIdxsp.size() == physMeshTransIdxSp.size());
		for (size_t i = 0; i < physIdxsp.size(); i++)
		{
			auto physMeshIt = meshes.ents.physObjs.find(physIdxsp[i]->meshIdx);
			if (physMeshIt != meshes.ents.physObjs.cend() && physMeshIt->second.mesh.Valid())
			{
				Vector pos = **physMeshTransIdxSp[i]->posIdx + landmarkDeltaToMapAtTick;
				QAngle ang = **physMeshTransIdxSp[i]->angIdx;
				mr.DrawMesh(physMeshIt->second.mesh,
				            [pos, ang](const CallbackInfoIn& infoIn, CallbackInfoOut& infoOut)
				            {
					            AngleMatrix(ang, pos, infoOut.mat);
					            // since the scale factor is small it should be fine that the OBB doesn't have the same fix
					            RenderCallbackZFightFix(infoIn, infoOut);
				            });
			}
			else
			{
				Assert(0);
			}
		}
	}
}

const Vector& TrRenderingCache::GetLandmarkOffsetToFirstMap(const char* fromMap)
{
	if (!fromMap)
		return vec3_origin;

	auto& tr = TrReadContextScope::Current();
	CacheLandmarkOffsetsToFirstMapFromTraceData();

	if (tr.Get<TrMap>().empty())
		return vec3_origin;

	auto [it, new_elem] = mapToFirstMapLandmarkOffset.try_emplace(fromMap);
	if (!new_elem)
		return it->second;

	/*
	* Iterate from the start of the trace and recompute the transition offset. At every map, check
	* if any of the landmarks match with the map we're loaded in. This is "slow", but I don't
	* expect more than a few transitions in the trace and a few landmarks for each map.
	* 
	* TODO: if the server is not active, we can't find landmarks and the cache will get borked. Fix that
	*/
	auto& landmarksInCurMap = utils::GetLandmarksInLoadedMap();
	TrIdx<TrMap> mapIdx = 0;
	Vector offFromFirst = vec3_origin;
	for (TrIdx<TrMapTransition> transitionIdx = 1u;; transitionIdx++)
	{
		for (auto& [curLandmarkName, curLandmarkOff] : landmarksInCurMap)
			for (auto& trLandmark : *mapIdx->landmarkSp)
				if (curLandmarkName == *trLandmark.nameIdx)
					return it->second = offFromFirst + curLandmarkOff - **trLandmark.posIdx;

		if (!transitionIdx.IsValid())
			break;

		offFromFirst -= tr.GetAdjacentLandmarkDelta(*transitionIdx->fromMapIdx->landmarkSp,
		                                            *transitionIdx->toMapIdx->landmarkSp);
		mapIdx = transitionIdx->toMapIdx;
	}
	return it->second = vec3_origin;
}

void TrRenderingCache::CacheLandmarkOffsetsToFirstMapFromTraceData()
{
	if (!mapToFirstMapLandmarkOffset.empty())
		return;

	auto& tr = TrReadContextScope::Current();
	if (tr.Get<TrMap>().empty())
		return;

	TrIdx<TrMap> mapIdx = 0;
	Vector offToFirstMap = vec3_origin;
	for (TrIdx<TrMapTransition> transitionIdx = 1u;; transitionIdx++)
	{
		mapToFirstMapLandmarkOffset[*mapIdx->nameIdx] = offToFirstMap;
		if (!transitionIdx.IsValid())
			break;
		offToFirstMap -= tr.GetAdjacentLandmarkDelta(*transitionIdx->fromMapIdx->landmarkSp,
		                                             *transitionIdx->toMapIdx->landmarkSp);
		mapIdx = transitionIdx->toMapIdx;
	}
}

void TrRenderingCache::RenderAll(MeshRendererDelegate& mr,
                                 const TrRenderEnableConfig& newEnableCfg,
                                 const TrRenderStyleConfig& newCfg,
                                 tr_tick atTick)
{
	auto& tr = TrReadContextScope::Current();

	pNewCfg = &newCfg;
	pNewEnableCfg = &newEnableCfg;

	if (renderedLastTimeOnMap != utils::GetLoadedMap())
	{
		mapToFirstMapLandmarkOffset.clear();
		renderedLastTimeOnMap = utils::GetLoadedMap();
	}

	atTick = tr.numRecordedTicks == 0 ? 0 : clamp(atTick, 0, tr.numRecordedTicks - 1);
	Vector landmarkdelta = GetLandmarkOffsetToFirstMap(utils::GetLoadedMap());
	if (newEnableCfg[TR_RENDER_ENABLE_PLAYER_PATH])
		RenderPlayerPath(mr, landmarkdelta);
	TrIdx<TrMap> atMap = tr.GetMapAtTick(atTick);
	if (atMap.IsValid())
	{
		landmarkdelta -= GetLandmarkOffsetToFirstMap(*atMap->nameIdx);
		if (newEnableCfg[TR_RENDER_ENABLE_PLAYER_HULL])
			RenderPlayerHull(mr, landmarkdelta, atTick);
		if (newEnableCfg[TR_RENDER_ENABLE_PORTALS])
			RenderPortals(mr, landmarkdelta, atTick);
		RenderEntities(mr, landmarkdelta, atTick);
	}

	tr.GetEntityCache().hoveredEnt.Invalidate();

	pNewCfg = nullptr;
	pNewEnableCfg = nullptr;
	enableCfg = newEnableCfg;
}

#endif
