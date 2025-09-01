#pragma once

#include <unordered_set>
#include <unordered_map>

#include "tr_structs.hpp"
#include "tr_entity_cache.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

namespace player_trace
{
	enum TrDrawFlags : uint32_t
	{
		TR_DRAW_NONE = 0,

		TR_DRAW_PLAYER_PATH = 1 << 0,
		TR_DRAW_PLAYER_PATH_CONES = 1 << 1, // only show if paired with DRAW_PLAYER_PATH
		TR_DRAW_PLAYER_HULL = 1 << 2,
		TR_DRAW_PLAYER_CONTACT_POINTS = 1 << 3, // only shown if paired with DRAW_PLAYER_HULL
		TR_DRAW_PORTALS = 1 << 4,
		TR_DRAW_PORTAL_COLLISION_ENTITIES = 1 << 5,
		TR_DRAW_ENTS = 1 << 6,
		TR_DRAW_ENT_COLLECT_RADIUS = 1 << 7,
	};

	enum TrPlayerCameraDrawType
	{
		TR_PCDT_FRUSTUM,
		TR_PCDT_BOX_AND_LINE,

		TR_PCDT_COUNT,
	};

	struct TrDrawParams
	{
		TrDrawFlags drawFlags = TR_DRAW_NONE;
		TrPlayerCameraDrawType camType = TR_PCDT_FRUSTUM;
		float eyeMeshFovOverride = -1; // -1 to use the FOV recorded in the trace
		tr_tick atTick = 0;
		bool drawIfTickOutsideRange = true;
	};

	/*
	* Anytime something changes trStyles or trColors, set the appropriate dirty flag here. That
	* will cause the relevant meshes to be rebuilt.
	*/
	struct TrRenderingCacheDirtyFlags
	{
		uint32_t playerPath : 1 = 1;
		uint32_t playerEyes : 1 = 1;
	} inline trRenderCacheDirtyFlags;

	class TrRenderingCache
	{
	private:
		struct TrDrawSettings
		{
			const TrDrawParams& params;
			Vector landmarkDeltaToFirstMap{std::numeric_limits<float>::infinity()};
		};

		std::unordered_map<std::string, Vector> mapToFirstMapLandmarkOffset;

		void RebuildPlayerHullMeshes();
		void RebuildEyeMeshes(TrPlayerCameraDrawType camType, float fov);
		void RebuildPlayerPathMeshes();
		void RebuildPortalMeshes();
		void RebuildPhysMeshes(const TrEntityCache::EntMap& entMap);

		void RenderPlayerPath(MeshRendererDelegate& mr, const TrDrawSettings& settings);
		void RenderPlayerHull(MeshRendererDelegate& mr, const TrDrawSettings& settings);
		void RenderPortals(MeshRendererDelegate& mr, const TrDrawSettings& settings);
		void RenderEntities(MeshRendererDelegate& mr, const TrDrawSettings& settings);
		void RenderEntCollectRadius(MeshRendererDelegate& mr, const TrDrawSettings& settings);

		/*
		* The player path coordinates are computed relative to the first map of the trace, but in order
		* to draw correctly in multi-map traces, we have to transform the whole thing.
		* 
		* Say we have the maps which are connected with landmarks: A <-> B <-> C <-> D
		* 
		* Case 1: we have a trace that goes in maps A->B:
		* - When you're in map A, no offset is applied.
		* - When you're in map B, an offset of A-B is applied to the path.
		* - When you're in map C, an offset of B-C is applied to the path (we can look at the landmarks in the current map).
		* - When you're in map D, no offset is applied since the trace does not have enough landmark data.
		* 
		* Case 2: we have a trace that is only in map A:
		* - When you're in map A, no offset is applied.
		* - When you're in map B, an offset of A-B is applied to the path.
		* - When you're in map C or D, no offset is applied since the trace does not have enough landmark data.
		* 
		* This means that different traces may get different map transforms applied depending on how
		* much landmark data they have. This is usually not gonna be a problem because if you're
		* loading the trace in a map two or more transitions away you're not gonna care about how it
		* lines up with the map.
		*/
		const Vector& GetLandmarkOffsetToFirstMap(const char* fromMap);
		void CacheLandmarkOffsetsToFirstMapFromTraceData();

		struct Meshes
		{
			StaticMesh qPhysDuck, qPhysStand;
			StaticMesh vPhysDuck, vPhysStand;
			StaticMesh eyes, sgEyes;
			StaticMesh openBluePortal, openOrangePortal, closedBluePortal, closedOrangePortal;

			struct
			{
				std::vector<StaticMesh> staticMeshes;
				std::vector<DynamicMesh> dynamicMeshes;
				tr_tick staticMeshesBuiltUpToTick = 0;
			} playerPath;

			struct EntityCache
			{
				struct TrackedMesh
				{
					StaticMesh mesh;
					bool isActive = false;
				};

				std::unordered_map<TrIdx<TrPhysMesh>, TrackedMesh> physObjs;
				bool anyStale = true;
			} ents;

		} meshes;

		std::string renderedLastTimeOnMap;

	public:
		TrRenderingCache() = default;
		TrRenderingCache(TrRenderingCache&) = delete;
		TrRenderingCache(TrRenderingCache&&) = delete;
		void RenderAll(MeshRendererDelegate& mr, const TrDrawParams& params);
	};

} // namespace player_trace

#endif
