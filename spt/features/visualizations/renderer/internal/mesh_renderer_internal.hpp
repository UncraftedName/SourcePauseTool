#pragma once

#include "mesh_builder_internal.hpp"
#include "..\mesh_renderer.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <memory_resource>
#include <variant>

struct MbUnitRenderData
{
	CallbackInfoOut cbInfoOut;
	MeshPositionInfo posInfo;
	Vector camDistSqrTo;
	float camDistSqr; // translucent sorting metric
	bool hasCallback;
};

struct MbStagingComponent
{
	MbComponent component;
	MbUnitRenderData renderData;

	bool IsDynamic() const
	{
		return component.IsDynamic();
	}

	friend std::weak_ordering operator<=>(const MbStagingComponent& a, const MbStagingComponent& b);
};

/*
* We already have mesh units which are already wrappers of internal mesh representations, so you may be wondering
* why we need another wrapper. In addition to the mesh units, this wrapper contains the user's render callback if
* they gave one, and the position metric to the unit. The user can give us multiple of the same mesh with
* different callbacks, so each one would have its own unit wrapper.
*/
struct MbMeshUnit
{
	// keep statics alive as long as we're rendering, dynamics are kept alive in the mesh builder
	const std::shared_ptr<StaticMeshUnit> staticUnit;
	const DynamicMeshToken dynamicToken;
	const RenderCallback callback;

	MbMeshUnit(DynamicMeshToken dynamicToken, const RenderCallback& callback)
	    : staticUnit{nullptr}, dynamicToken{dynamicToken}, callback{callback}
	{
	}

	MbMeshUnit(const std::shared_ptr<StaticMeshUnit>& staticUnit, const RenderCallback& callback)
	    : staticUnit{staticUnit}, dynamicToken{}, callback{callback}
	{
	}

	MbMeshUnit(MbMeshUnit&) = delete;

	bool IsDynamic() const
	{
		return !staticUnit;
	}

	// returns true if this unit should be rendered
	std::optional<MbUnitRenderData> GetRenderData() const;
};

struct MeshRendererInternal
{
	std::pmr::memory_resource& mr;

	std::pmr::vector<MbDynamicMeshUnit> dynamicUnits;
	// TODO change (most) lists to forward lists?
	std::pmr::list<MbMeshUnit> queuedUnits;

	bool acceptUserData = false;
	bool renderingSkyBox = false;

	struct
	{
		size_t nDynamicsThisFrame = 0;
		size_t nStaticsThisFrame = 0;
	} stats;

	struct
	{
		const CRendering3dView* rendering3dView = nullptr;
		const CViewSetup* viewSetup = nullptr;
		cplane_t frustum[FRUSTUM_NUMPLANES]{};
	} viewInfo;

	const MbDynamicMeshUnit& GetDynamicMeshFromToken(DynamicMeshToken token) const
	{
		return dynamicUnits[token.dynamicMeshIdx];
	}

	struct DebugBox
	{
		Vector mins, maxs;
	};

	struct DebugCross
	{
		Vector crossPos;
		float size;
	};

	struct DebugMeshDesc
	{
		std::variant<DebugBox, DebugCross> desc;
		color32 color;
	};

	struct DebugMeshesForView
	{
		std::pmr::vector<DebugMeshDesc> descs;

		DebugMeshesForView(std::pmr::memory_resource& mr) : descs{&mr} {}
	};

	std::pmr::list<DebugMeshesForView> debugMeshViews;

	/*
	* For simplicity, we make debug meshes not do z-testing. If we do this we can queue debug meshes in
	* DrawOpaques() & DrawTranslucents() and then draw all of them at the very end. Suppose we have 3 portals
	* A,B,C that we can look through, and we can see portal C through portal B. Then the call stack of the
	* drawing functions sort of looks like this:
	* 
	* - DrawOpaques()      (main view)
	*   - DrawOpaques()      (view through portal A)
	*   - DrawTranslucents() (view through portal A)
	*   - DrawOpaques()      (view through portal B)
	*     - DrawOpaques()      (view through portal C)
	*     - DrawTranslucents() (view through portal C)
	*   - DrawTranslucents() (view through portal B)
	* - DrawTranslucents() (main view)
	* 
	* (This is a lie because the calls are actually recursed from DrawTranslucents() in game but we draw our
	* translucent stuff after the game so for the purpose of debug meshes our stack looks like ^that^).
	* 
	* If debug meshes were opaque, we would need to draw them in DrawOpaques(), so we'd need to know about all
	* translucent meshes *before* they get rendererd. This would require a more complicated queueing system.
	* 
	* Currently, when we start in DrawOpaques(), we'll "push" a debug slice to the debug description list, then we
	* add all opaque debug meshes to that slice. Then we recursively get all the portal views, and once we're done
	* with those we'll get to the corresponding DrawTranslucents() on the same depth as the DrawOpaques() call.
	* Once we're done with that, the top most slice will have all debug meshes for the current view, and we can
	* batch all of them together at the end of DrawTranslucents().
	*/

	MeshRendererInternal(std::pmr::memory_resource& mr)
	    : mr(mr), dynamicUnits(&mr), queuedUnits(&mr), debugMeshViews(&mr)
	{
	}

	void OnDrawOpaques(CRendering3dView* rendering3dView);
	void OnDrawTranslucents(CRendering3dView* rendering3dView);

	void SetupViewInfo(CRendering3dView* rendering3dView);
	void CollectRenderableComponents(std::pmr::list<MbStagingComponent>& components, bool opaques);
	void DrawAll(std::span<const std::reference_wrapper<MbStagingComponent>> span,
	             bool addDebugMeshes,
	             bool opaques);

	void AddDebugCrosses(std::pmr::list<MbStagingComponent>& components);
	void AddDebugBoxesForFusedDynamic(std::span<const std::reference_wrapper<MbStagingComponent>> span,
	                                  bool opaques);
	void AddDebugBoxForStatic(const MbStagingComponent& component);
	void DrawDebugMeshesForCurrentView();

	void Render(const IMeshWrapper& mw, const MbUnitRenderData& renderData);
};

// clang-format off

// a memory resource that counts how many bytes it has allocated
class MbAllocCountingResource final : public std::pmr::memory_resource
{
public:
	std::pmr::memory_resource& upstream;
	std::atomic<size_t> nTotalBytesAlloc = 0;
	MbAllocCountingResource(std::pmr::memory_resource& upstream = *std::pmr::get_default_resource()) : upstream{upstream} {}
protected:
	void* do_allocate(std::size_t bytes, std::size_t alignment) override { nTotalBytesAlloc += bytes; return upstream.allocate(bytes, alignment); }
	void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override { upstream.deallocate(p, bytes, alignment); }
	bool do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }
};

// clang-format on

struct MeshRendererFeature::FrameDataImpl
{
	/*
	* Order of these members matters! We first initialize the arena (monotonic_buffer) using an
	* initial buffer, then initialize the renderer with the arena. We have to allocate our own
	* initial buffer because otherwise the counting resource will count the first allocation
	* towards the total number of bytes. This would cause the number of bytes allocated to never
	* decrease over multiple frames.
	*/
	std::pmr::monotonic_buffer_resource outerArena;
	MbAllocCountingResource outerCountingResource;
	std::pmr::monotonic_buffer_resource innerArena;
	MbAllocCountingResource innerCountingResource;
	std::pmr::memory_resource& mr;
	MeshRendererInternal renderer;

	FrameDataImpl(size_t initNBytesAlloc)
	    : outerArena{initNBytesAlloc}
	    , outerCountingResource{outerArena}
	    , innerArena{&outerCountingResource}
	    , innerCountingResource{innerArena}
	    , mr{innerCountingResource}
	    , renderer{mr}
	{
	}
};

struct MbParanoidAllocScope
{
#ifdef DEBUG
	class AssertMemResource : public std::pmr::memory_resource
	{
	protected:
		void* do_allocate(std::size_t, std::size_t) override
		{
			Assert(0);
			throw std::bad_alloc{};
		}

		void do_deallocate(void*, std::size_t, std::size_t) override
		{
			Assert(0);
			throw std::bad_alloc{};
		}

		bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override
		{
			return dynamic_cast<const AssertMemResource*>(&other) != nullptr;
		}
	} assertMemResource;

	std::pmr::memory_resource* oldResource;

	MbParanoidAllocScope() : oldResource{std::pmr::get_default_resource()}
	{
		std::pmr::set_default_resource(&assertMemResource);
	}

	~MbParanoidAllocScope()
	{
		std::pmr::set_default_resource(oldResource);
	}
#endif
};

#endif
