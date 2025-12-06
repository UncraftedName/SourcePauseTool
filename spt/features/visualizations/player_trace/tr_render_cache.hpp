#pragma once

#include "tr_structs.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "tr_entity_cache.hpp"
#include "spt/features/visualizations/renderer/mesh_builder.hpp"

#include <unordered_set>
#include <unordered_map>

class MeshRendererDelegate;

namespace player_trace
{

	// to check if the cache is dirty I'm using memcmp - so no implicit padding
#pragma warning(push)
#pragma warning(error : 4820)

	enum TrPlayerCameraDrawType : unsigned char
	{
		TR_PCDT_FRUSTUM,
		TR_PCDT_BOX_AND_LINE,

		TR_PCDT_COUNT,
	};

#define _TR_RSC_EXPAND_LC_X(X, linecolor) X(linecolor.lineColor)

#define _TR_RSC_EXPAND_SC_X(X, shapecolor) \
	X(shapecolor.faceColor) \
	X(shapecolor.lineColor)

#define _TRSC_EXPAND_SBC_X(X, sweptboxcolor) \
	_TR_RSC_EXPAND_SC_X(X, sweptboxcolor.cStart) \
	_TR_RSC_EXPAND_SC_X(X, sweptboxcolor.cSweep) \
	_TR_RSC_EXPAND_SC_X(X, sweptboxcolor.cEnd)

	// big fat god struct with macros to tint all colors at once
	struct TrRenderStyleConfig
	{
		struct PlayerPath
		{
			bool draw = true;
			char _pad0[3]{0};

			struct Cones
			{
				bool draw = true;
				char _pad0[3]{0};
				float opacity = .2f;
				int nCirclePoints = 5;
				float length = 3.f;
				float radius = 0.7f;
				int tickInterval = 10;
			} cones;

			struct Endpoints
			{
				float opacity = 0.2f;
				int nCirclePoints = 20;
				float radius = 4.f;

				bool draw = true;

				std::array<ShapeColor, TR_SR_COLORED_COUNT> colors{
				    ShapeColor{C_OUTLINE(200, 100, 100, 255), true, true, WD_BOTH},
				    ShapeColor{C_OUTLINE(100, 100, 200, 255), true, true, WD_BOTH},
				    ShapeColor{C_OUTLINE(100, 200, 100, 255), true, true, WD_BOTH},
				    ShapeColor{C_OUTLINE(200, 200, 100, 255), true, true, WD_BOTH},
				    ShapeColor{C_OUTLINE(200, 200, 200, 255), true, true, WD_BOTH},
				};
			} endpoints;

			struct Segments
			{
				float maxDistBeforeImplicitBreakSqr = 130.f * 130.f; // max speed + crouch spamming
				uint32_t maxTicksToRenderAsDynamicMesh = 1000;

				// TODO implement me
				struct
				{
					LineColor grounded{color32{255, 255, 255, 255}};
					LineColor air{color32{0, 255, 0, 255}};
					LineColor airSoftSpeedLocked{color32{200, 200, 0, 255}};
					LineColor airHardSpeedLocked{color32{100, 100, 0, 255}};
				} colors;
			} segments;
		} playerPath;

		static_assert(std::tuple_size<decltype(playerPath.endpoints.colors)>{} == 5);

#define TR_RSC_ENUMERATE_PLAYER_PATH_COLS_X(X) \
	_TR_RSC_EXPAND_SC_X(X, playerPath.endpoints.colors[0]) \
	_TR_RSC_EXPAND_SC_X(X, playerPath.endpoints.colors[1]) \
	_TR_RSC_EXPAND_SC_X(X, playerPath.endpoints.colors[2]) \
	_TR_RSC_EXPAND_SC_X(X, playerPath.endpoints.colors[3]) \
	_TR_RSC_EXPAND_SC_X(X, playerPath.endpoints.colors[4]) \
	_TR_RSC_EXPAND_LC_X(X, playerPath.segments.colors.grounded) \
	_TR_RSC_EXPAND_LC_X(X, playerPath.segments.colors.air) \
	_TR_RSC_EXPAND_LC_X(X, playerPath.segments.colors.airSoftSpeedLocked) \
	_TR_RSC_EXPAND_LC_X(X, playerPath.segments.colors.airHardSpeedLocked)

		struct PlayerHull
		{
			float originCubeSize = 1.f;
			float qPhysCenterCrossRadius = 5.f;
			bool drawOriginCube = true;
			bool drawQPhysCenter = true;
			bool draw = true;
			char _pad0[1]{0};

			struct
			{
				/*
				* Colors pulled from:
				* - CHL2_Player::DrawDebugGeometryOverlays
				* - CBaseEntity::DrawDebugGeometryOverlays
				* - DebugDrawContactPoints
				*/
				ShapeColor qPhys = ShapeColor{C_WIRE(255, 100, 0, 255)};
				ShapeColor qPhysReduction = ShapeColor{C_OUTLINE(255, 0, 0, 100)};
				ShapeColor qPhysOrigin = ShapeColor{C_OUTLINE(255, 100, 200, 50)};
				ShapeColor vPhys = ShapeColor{C_OUTLINE(255, 255, 0, 16)};
			} colors;
		} playerHull;

#define TR_RSC_ENUMERATE_PLAYER_HULL_COLS_X(X) \
	_TR_RSC_EXPAND_SC_X(X, playerHull.colors.qPhys) \
	_TR_RSC_EXPAND_SC_X(X, playerHull.colors.qPhysReduction) \
	_TR_RSC_EXPAND_SC_X(X, playerHull.colors.qPhysOrigin) \
	_TR_RSC_EXPAND_SC_X(X, playerHull.colors.vPhys)

		struct ContactPoint
		{
			float normalLength = 20.f;
			bool draw = true;
			ShapeColor color{C_OUTLINE(0, 255, 0, 32)};
		} contactPoints;

#define TR_RSC_ENUMERATE_CONTACTS_COLS_X(X) _TR_RSC_EXPAND_SC_X(X, contactPoints.color)

		struct PlayerEye
		{
			enum EyeType : unsigned char
			{
				TR_EYE_ACTUAL,
				TR_EYE_SAVEGLITCH,

				TR_EYE_COUNT,
			};

			struct Frustum
			{
				float zFar = 8.f;
				float aspect = 16 / 9.f;
				float hatFloat = 1.f;
				float hatHeight = 3.f;

				std::array<ShapeColor, TR_EYE_COUNT> colors{
				    ShapeColor{C_OUTLINE(20, 200, 20, 50)},
				    ShapeColor{C_OUTLINE(100, 200, 20, 50)},
				};

				char _pad0[2]{0};
			} frustum;

			struct boxAndLine
			{
				float boxRadius = 1.f;
				float lineLength = 8.f;

				std::array<ShapeColor, TR_EYE_COUNT> colors{
				    ShapeColor{C_FACE(0, 255, 255, 255)},
				    ShapeColor{C_FACE(100, 200, 255, 255)},
				};
				char _pad0[2]{0};
			} boxAndLine;

			TrPlayerCameraDrawType style = TR_PCDT_FRUSTUM;
			char _pad0[3]{0};
		} playerEye;

#define TR_RSC_ENUMERATE_EYE_COLS_X(X) \
	_TR_RSC_EXPAND_SC_X(X, playerEye.frustum.colors[0]) \
	_TR_RSC_EXPAND_SC_X(X, playerEye.frustum.colors[1]) \
	_TR_RSC_EXPAND_SC_X(X, playerEye.boxAndLine.colors[0]) \
	_TR_RSC_EXPAND_SC_X(X, playerEye.boxAndLine.colors[1])

		struct Portals
		{
			bool draw = true;
			char _pad0[3]{0};
			float portalThickness = 4.f;
			ArrowParams arrowParams{5, 10.f, 1.f, .3f, 2.f};
			float hatFloat = 3.f;
			float hatHeight = 8.f;

			struct
			{
				ShapeColor blue{C_OUTLINE(64, 160, 255, 127)};
				ShapeColor orange{C_OUTLINE(255, 160, 32, 127)};
			} colors;

			char _pad1[2]{0};
		} portals;

#define TR_RSC_ENUMERATE_PORTAL_COLS_X(X) \
	_TR_RSC_EXPAND_SC_X(X, portals.colors.blue) \
	_TR_RSC_EXPAND_SC_X(X, portals.colors.orange)

		struct EntPhys
		{
			int nBallMeshSubdivisions = 4;
			bool draw = true;
			ShapeColor color{C_OUTLINE(200, 60, 100, 20)};

			struct
			{
				bool draw = false;
				ShapeColor color{C_OUTLINE(200, 20, 200, 50)};
			} portalCollisionEnts;
		} entPhys;

#define TR_RSC_ENUMERATE_ENT_PHYS_COLS_X(X) \
	_TR_RSC_EXPAND_SC_X(X, entPhys.color) \
	_TR_RSC_EXPAND_SC_X(X, entPhys.portalCollisionEnts.color)

		struct EntObb
		{
			bool draw = true;
			bool enableCenterCross = true;
			bool enableTriggers = true;
			char _pad0[1]{0};

			float centerCrossRadius = 4.f;

			ShapeColor color{C_WIRE(220, 150, 20, 255)};
			ShapeColor colorTrigger{C_OUTLINE(200, 200, 20, 10)};
			ShapeColor colorHovered{C_OUTLINE(255, 0, 0, 100), false, false, WD_BOTH};
			char _pad1[3]{0};
		} entObb;

#define TR_RSC_ENUMERATE_ENT_OBB_COLS_X(X) \
	_TR_RSC_EXPAND_SC_X(X, entObb.color) \
	_TR_RSC_EXPAND_SC_X(X, entObb.colorTrigger) \
	_TR_RSC_EXPAND_SC_X(X, entObb.colorHovered)

		struct EntCollectAabb
		{
			bool draw = true;
			ShapeColor color{C_WIRE(150, 100, 100, 255)};
		} entCollectAabb;

#define TR_RSC_ENUMERATE_ENT_COLLECT_COLS_X(X) _TR_RSC_EXPAND_SC_X(X, entCollectAabb.color)

#define TR_RSC_ENUMERATE_COLS_X(X) \
	TR_RSC_ENUMERATE_PLAYER_PATH_COLS_X(X) \
	TR_RSC_ENUMERATE_PLAYER_HULL_COLS_X(X) \
	TR_RSC_ENUMERATE_CONTACTS_COLS_X(X) \
	TR_RSC_ENUMERATE_EYE_COLS_X(X) \
	TR_RSC_ENUMERATE_PORTAL_COLS_X(X) \
	TR_RSC_ENUMERATE_ENT_PHYS_COLS_X(X) \
	TR_RSC_ENUMERATE_ENT_OBB_COLS_X(X) \
	TR_RSC_ENUMERATE_ENT_COLLECT_COLS_X(X)

		void Multiply(color32 tint)
		{
#define _TR_RSC_TINT_X(color) color = color32Mult(color, tint);
			TR_RSC_ENUMERATE_COLS_X(_TR_RSC_TINT_X);
#undef _TR_RSC_TINT_X
		}
	};

#pragma warning(pop)

	/*
	* This is the thing which renders the trace. Each trace has an associated rendering cache as a
	* field. This builds up mostly static meshes and caches them for future calls. If the style of
	* anything is updated, the cached meshes are rebuilt.
	*/
	class TrRenderingCache
	{
	private:
		std::unordered_map<std::string, Vector> mapToFirstMapLandmarkOffset;
		// we keep a copy of the config and memcmp/memcpy against the one passed in to DrawAll
		TrRenderStyleConfig cfg{};

		const TrRenderStyleConfig* pNewCfg = nullptr; // the current cfg passed to DrawAll

		void RebuildPlayerHullMeshes();
		void RebuildEyeMeshes(float fov);
		void RebuildPlayerPathMeshes();
		void RebuildPortalMeshes();
		void RebuildPhysMeshes(tr_tick atTick);

		void RenderPlayerPath(MeshRendererDelegate& mr, const Vector& landmarkDeltaToFirstMap);
		void RenderPlayerHull(MeshRendererDelegate& mr, const Vector& landmarkDeltaToMapAtTick, tr_tick atTick);
		void RenderPortals(MeshRendererDelegate& mr, const Vector& landmarkDeltaToMapAtTick, tr_tick atTick);
		void RenderEntities(MeshRendererDelegate& mr, const Vector& landmarkDeltaToMapAtTick, tr_tick atTick);
		void RenderEntObbs(MeshRendererDelegate& mr, const Vector& landmarkDeltaToMapAtTick, tr_tick atTick);
		void RenderEntPhysMeshes(MeshRendererDelegate& mr,
		                         const Vector& landmarkDeltaToMapAtTick,
		                         tr_tick atTick);

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

			float lastEyeMeshFov = -1;

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
		TrRenderingCache(const TrRenderingCache&) = delete;
		void RenderAll(MeshRendererDelegate& mr, const TrRenderStyleConfig& newCfg, tr_tick atTick);
	};

} // namespace player_trace

#endif
