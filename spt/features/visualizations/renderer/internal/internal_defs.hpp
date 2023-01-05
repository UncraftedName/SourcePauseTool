/*
* This file functions as the header for the internal mesh builder and material manager, and also contains
* constants/typedefs used by the whole rendering system. Like the mesh renderer, there are 3 mesh builders:
* - MeshBuilderPro (spt_meshBuilder)
* - MeshBuilderDelegate
* - MeshBuilderInternal (g_meshBuilderInternal)
* The MeshBuilderPro is stateless, and will give the user a stateless delegate when they want to create a mesh.
* The user then calls the various methods of the delegate which will edit the state of the internal builder.
* 
* At the end of the day, the mesh builder's purpose is to create IMesh* objects so that the renderer can call
* IMesh->Draw(). For static meshes, this is simple enough - the user can fill up the builder's buffers with data,
* and when they're done we can call IMatRenderContext::CreateStaticMesh(...). The IMesh* is then yeeted onto the
* heap and given to the renderer later. The life cycle for static meshes looks like this:
* 
* 1) The user calls MeshBuilderPro::CreateStaticMesh(...)
* 2) The MeshBuiderPro gives the user a MeshBuilderDelegate
* 3) The MeshBuilderDelegate edits the state of the MeshBuilderInternal, populating its current MeshVertData
* 4) The MeshBuilderPro asks the MeshBuilderInternal to create an IMesh* object from the MeshVertData
* 5) The MeshBuilderPro puts the IMesh* object into the heap (wrapped in a StaticMesh) and gives that to the user
* 6) The user can then destroy or give that StaticMesh to the MeshRendererFeature
* 
* Dynamic meshes are a little bit more complicated. Under the hood (in game code) there is only one dynamic mesh,
* and we simply edit its state. However, that means that only a single dynamic IMesh* object can exist at a time.
* This means that we must keep the instructions to build dynamic meshes for the entire frame, and recreate IMesh*
* objects just as they're about to be drawn. The life cycle for dynamic meshes looks like this:
* 
* 1) The user calls MeshBuilderPro::CreateDynamicMesh(...)
* 2) Same as above
* 3) Same as above
* 4) The MeshBuilderPro asks to the MeshBuilderInternal to save the current MeshVertData
* 5) The MeshBuilderPro gives the user a token for that data (only internal classes can use it)
* 6) The user can give that token to the renderer
* 7) The renderer asks the MeshBuilderInternal to create the IMesh* object when it's about to draw it (every time)
* 8) At the start of the next frame, the MeshBuilderInternal clears its MeshVertData arrays
* 
* As such, the user is never responsible for the destruction of dynamic meshes. There are a few optimizations,
* probably the most complicated of which being the fusing of dynamic meshes. When asking the MeshBuilderInternal
* for an IMesh*, you provide not just 1 but a whole interval of meshes, then the builder will automatically fuse
* consecutive elements and give you one IMesh* object at a time until it's done iterating over that interval.
*/

#pragma once

#include "..\mesh_defs.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include "mathlib\vector.h"
#include "materialsystem\imaterial.h"

#pragma warning(push)
#pragma warning(disable : 5054)
#include "materialsystem\imesh.h"
#pragma warning(pop)

#define VPROF_LEVEL 1
#ifndef SSDK2007
#define RAD_TELEMETRY_DISABLED
#endif
#include "vprof.h"

#define VPROF_BUDGETGROUP_MESHBUILDER _T("Mesh_Builder")
#define VPROF_BUDGETGROUP_MESH_RENDERER _T("Mesh_Renderer")

#include <stack>
#include <Windows.h>

// used to check if dynamic mesh tokens are valid, increases every frame
inline int g_meshRenderFrameNum = 0;
inline bool g_inMeshRenderSignal = false;

using VertIndex = unsigned short;
using DynamicMeshToken = DynamicMesh;

template<class T>
using VectorStack = std::stack<T, std::vector<T>>;

inline void MsgHResultError(HRESULT hr, const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	char fmtBuf[1024];
	vsnprintf(fmtBuf, sizeof fmtBuf, fmt, args);
	va_end(args);

	if (FAILED(hr))
	{
		Warning("spt: %s: success\n", fmtBuf);
	}
	else
	{
		LPTSTR errorMsg = NULL;
		FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
		                   | FORMAT_MESSAGE_IGNORE_INSERTS,
		               NULL,
		               hr,
		               MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
		               (LPTSTR)&errorMsg,
		               0,
		               NULL);
		if (errorMsg != NULL)
		{
			Warning("spt: %s: %s\n", fmtBuf, errorMsg);
			LocalFree(errorMsg);
		}
		else
		{
			Warning("spt: %s: unknown error\n", fmtBuf);
		}
	}
}

#endif
