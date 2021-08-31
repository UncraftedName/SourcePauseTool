#pragma once

/*
 * The DebugOverlay is cool and all, but it's terribly slow for drawing stuff that has hundreds/thousands of triangles.
 * Implementations of visualizations that draw actual meshes or complex geometry should be done here. Currently this
 * is done is by calling the DebugDrawPhysCollide method and hooking the CreateDebugMesh function which is called from
 * there, so you can only draw CPhysCollides/CPhysicsObjects. That being said, it should be easy to hook the
 * Create/Destroy DebugMesh functions so that way you can draw arbtirary meshes when you call DebugDrawPhysCollide.
 * Note that unlike the DebugOverlay, DebugDrawPhysCollide needs to be called at a specific time during rendering,
 * see: ClientDLL::HOOKED_CRendering3dView__DrawTranslucentRenderables.
 */

void DrawSgCollision();
