#include "stdafx.h"

#include "..\mesh_defs_private.hpp"

#ifdef SPT_MESH_RENDERING_ENABLED

#include <memory>

#include "..\mesh_builder.hpp"
#include "spt\utils\math.hpp"

Vector* Scratch(size_t n)
{
	static std::unique_ptr<Vector[]> scratch = nullptr;
	static size_t count = 0;
	if (count < n)
	{
		count = SmallestPowerOfTwoGreaterOrEqual(n);
		scratch.reset(new Vector[count]);
	}
	return scratch.get();
}

#define INDEX_COUNT_MULTIPLIER(wd) (((wd)&WD_BOTH) == WD_BOTH ? 2 : 1)

#define INVERT_WD(wd) (((wd)&WD_BOTH) == WD_BOTH ? WD_BOTH : (WindingDir)((wd & WD_BOTH) ^ WD_BOTH))

#define GET_VDATA_FACES_CUSTOM_MATERIAL(material) \
	g_meshBuilderInternal.FindOrAddVData(MeshPrimitiveType::Triangles, material)

#define GET_VDATA_LINES_CUSTOM_MATERIAL(material) \
	g_meshBuilderInternal.FindOrAddVData(MeshPrimitiveType::Lines, material)

#define BASIC_MATERIAL(color, zTest) \
	((zTest) ? ((color).a == 255 ? g_meshMaterialMgr.matOpaque : g_meshMaterialMgr.matAlpha) \
	         : g_meshMaterialMgr.matAlphaNoZ)

#define GET_VDATA_FACES(color, zTest) GET_VDATA_FACES_CUSTOM_MATERIAL(BASIC_MATERIAL(color, zTest))
#define GET_VDATA_LINES(color, zTest) GET_VDATA_LINES_CUSTOM_MATERIAL(BASIC_MATERIAL(color, zTest))

#define DECLARE_MULTIPLE_COMPONENTS(count) g_meshBuilderInternal.curMeshVertData.reserve_extra(count)

// TODO change me to a macro, remove dtor
// Instead of using data->Reserve directly - give the numbers to this struct and it will check that
// your numbers are correct when it goes out of scope.
struct ReserveScope
{
	MeshVertData& vd;
	const size_t origNumVerts, origNumIndices;
	const size_t expectedExtraVerts, expectedExtraIndices;
	inline static int depth[(int)MeshPrimitiveType::Count] = {};

	ReserveScope(MeshVertData& vd, size_t numExtraVerts, size_t numExtraIndices)
	    : vd(vd)
	    , origNumVerts(vd.verts.size())
	    , origNumIndices(vd.indices.size())
	    , expectedExtraVerts(numExtraVerts)
	    , expectedExtraIndices(numExtraIndices)
	{
		if (depth[(int)vd.type]++ == 0)
		{
			vd.verts.reserve_extra(numExtraVerts);
			vd.indices.reserve_extra(numExtraIndices);
		}
	}

	~ReserveScope()
	{
		depth[(int)vd.type]--;
		size_t actualNewVerts = vd.verts.size() - origNumVerts;
		size_t actualNewIndices = vd.indices.size() - origNumIndices;
		(void)actualNewVerts;
		(void)actualNewIndices;
		AssertMsg2(actualNewVerts == expectedExtraVerts,
		           "Expected %u extra verts, got %u",
		           expectedExtraVerts,
		           actualNewVerts);
		AssertMsg2(actualNewIndices == expectedExtraIndices,
		           "Expected %u extra indices, got %u",
		           expectedExtraIndices,
		           actualNewIndices);
	}
};

void MeshBuilderDelegate::_AddLine(const Vector& v1, const Vector& v2, color32 c, MeshVertData& vdl)
{
	ReserveScope rs(vdl, 2, 2);
	vdl.indices.push_back(vdl.verts.size());
	vdl.indices.push_back(vdl.verts.size() + 1);
	vdl.verts.emplace_back(v1, c);
	vdl.verts.emplace_back(v2, c);
}

void MeshBuilderDelegate::AddLine(const Vector& v1, const Vector& v2, color32 c, bool zTest)
{
	if (c.a == 0)
		return;
	_AddLine(v1, v2, c, GET_VDATA_LINES(c, zTest));
}

void MeshBuilderDelegate::AddLines(const Vector* points, int nSegments, color32 c, bool zTest)
{
	if (!points || nSegments <= 0 || c.a == 0)
		return;
	auto& vdl = GET_VDATA_LINES(c, zTest);
	ReserveScope rs(vdl, nSegments * 2, nSegments * 2);
	for (int i = 0; i < nSegments * 2; i++)
	{
		vdl.indices.push_back(vdl.verts.size());
		vdl.verts.emplace_back(points[i], c);
	}
}

// TODO remove the reserve scope from here and other _methods?
void MeshBuilderDelegate::_AddLineStrip(const Vector* points, int nPoints, bool loop, color32 c, MeshVertData& vdl)
{
	if (!points || nPoints < 2)
		return;
	ReserveScope rs(vdl, nPoints, (nPoints - 1 + loop) * 2);
	for (int i = 0; i < nPoints; i++)
		vdl.verts.emplace_back(points[i], c);
	_AddLineStripIndices(vdl, vdl.verts.size() - nPoints, nPoints, loop);
}

void MeshBuilderDelegate::AddLineStrip(const Vector* points, int nPoints, bool loop, color32 c, bool zTest)
{
	if (c.a == 0)
		return;
	_AddLineStrip(points, nPoints, loop, c, GET_VDATA_LINES(c, zTest));
}

void MeshBuilderDelegate::AddCross(const Vector& pos, float radius, color32 c, bool zTest)
{
	if (c.a == 0 || radius <= 0)
		return;
	static float axf = powf(1.f / 3, 0.5f);
	auto& vdl = GET_VDATA_LINES(c, zTest);
	ReserveScope rs(vdl, 8, 8);
	for (int x = -1; x <= 1; x += 2)
	{
		for (int y = -1; y <= 1; y += 2)
		{
			Vector v = Vector{x * axf, y * axf, axf} * radius;
			_AddLine(-v + pos, v + pos, c, vdl);
		}
	}
}

void MeshBuilderDelegate::AddTri(const Vector& v1,
                                 const Vector& v2,
                                 const Vector& v3,
                                 MeshColor mc,
                                 bool zTest,
                                 WindingDir wd)
{
	Vector v[] = {v1, v2, v3};
	AddTris(v, 1, mc, zTest, wd);
}

void MeshBuilderDelegate::AddTris(const Vector* verts, int nFaces, MeshColor mc, bool zTest, WindingDir wd)
{
	if (!verts || nFaces <= 0 || !(wd & WD_BOTH))
		return;

	if (mc.faceColor.a != 0)
	{
		auto& vdf = GET_VDATA_FACES(mc.faceColor, zTest);
		ReserveScope rs(vdf, nFaces * 3, nFaces * 3 * INDEX_COUNT_MULTIPLIER(wd));
		for (int i = 0; i < nFaces; i++)
		{
			size_t vIdx = vdf.verts.size();
			vdf.verts.emplace_back(verts[3 * i + 0], mc.faceColor);
			vdf.verts.emplace_back(verts[3 * i + 1], mc.faceColor);
			vdf.verts.emplace_back(verts[3 * i + 2], mc.faceColor);
			if (wd & WD_CW)
			{
				vdf.indices.push_back(vIdx + 0);
				vdf.indices.push_back(vIdx + 1);
				vdf.indices.push_back(vIdx + 2);
			}
			if (wd & WD_CCW)
			{
				vdf.indices.push_back(vIdx + 2);
				vdf.indices.push_back(vIdx + 1);
				vdf.indices.push_back(vIdx + 0);
			}
		}
	}

	if (mc.lineColor.a != 0)
	{
		auto& vdl = GET_VDATA_LINES(mc.lineColor, zTest);
		ReserveScope rs(vdl, nFaces * 3, nFaces * 6);
		for (int i = 0; i < nFaces; i++)
			_AddLineStrip(verts + 3 * i, 3, true, mc.lineColor, vdl);
	}
}

void MeshBuilderDelegate::AddQuad(const Vector& v1,
                                  const Vector& v2,
                                  const Vector& v3,
                                  const Vector& v4,
                                  MeshColor mc,
                                  bool zTest,
                                  WindingDir wd)
{
	Vector v[] = {v1, v2, v3, v4};
	AddPolygon(v, 4, mc);
}

// don't expect this to be used much
void MeshBuilderDelegate::AddQuads(const Vector* verts, int nFaces, MeshColor mc, bool zTest, WindingDir wd)
{
	if (!verts || nFaces <= 0 || !(wd & WD_BOTH))
		return;
	DECLARE_MULTIPLE_COMPONENTS(2);
	auto& vdf = GET_VDATA_FACES(mc.faceColor, zTest);
	auto& vdl = GET_VDATA_LINES(mc.lineColor, zTest);
	for (int i = 0; i < nFaces; i++)
		_AddPolygon(verts + i * 4, 4, mc, wd, vdf, vdl);
}

void MeshBuilderDelegate::_AddPolygon(const Vector* verts,
                                      int nVerts,
                                      MeshColor mc,
                                      WindingDir wd,
                                      MeshVertData& vdf,
                                      MeshVertData& vdl)
{
	if (!verts || nVerts < 3 || !(wd & WD_BOTH))
		return;
	if (mc.faceColor.a != 0)
	{
		ReserveScope rs(vdf, nVerts, (nVerts - 2) * 3 * INDEX_COUNT_MULTIPLIER(wd));
		for (int i = 0; i < nVerts; i++)
			vdf.verts.emplace_back(verts[i], mc.faceColor);
		_AddFacePolygonIndices(vdf, vdf.verts.size() - nVerts, nVerts, wd);
	}
	if (mc.lineColor.a != 0)
		_AddLineStrip(verts, nVerts, true, mc.lineColor, vdl);
}

void MeshBuilderDelegate::AddPolygon(const Vector* verts, int nVerts, MeshColor mc, bool zTest, WindingDir wd)
{
	DECLARE_MULTIPLE_COMPONENTS(2);
	_AddPolygon(verts, nVerts, mc, wd, GET_VDATA_FACES(mc.faceColor, zTest), GET_VDATA_LINES(mc.lineColor, zTest));
}

void MeshBuilderDelegate::AddCircle(const Vector& pos,
                                    const QAngle& ang,
                                    float radius,
                                    int nPoints,
                                    MeshColor mc,
                                    bool zTest,
                                    WindingDir wd)
{
	AddEllipse(pos, ang, radius, radius, nPoints, mc, zTest, wd);
}

void MeshBuilderDelegate::AddEllipse(const Vector& pos,
                                     const QAngle& ang,
                                     float radiusA,
                                     float radiusB,
                                     int nPoints,
                                     MeshColor mc,
                                     bool zTest,
                                     WindingDir wd)
{
	if (nPoints < 3 || radiusA < 0 || radiusB < 0 || (mc.faceColor.a == 0 && mc.lineColor.a == 0)
	    || !(wd & WD_BOTH))
	{
		return;
	}
	AddPolygon(_CreateEllipseVerts(pos, ang, radiusA, radiusB, nPoints), nPoints, mc, zTest, wd);
}

void MeshBuilderDelegate::AddBox(const Vector& pos,
                                 const Vector& mins_,
                                 const Vector& maxs_,
                                 const QAngle& ang,
                                 MeshColor mc,
                                 bool zTest,
                                 WindingDir wd)
{
	if ((mc.faceColor.a == 0 && mc.lineColor.a == 0) || !(wd & WD_BOTH))
		return;

	DECLARE_MULTIPLE_COMPONENTS(2);
	auto& vdf = GET_VDATA_FACES(mc.faceColor, zTest);
	auto& vdl = GET_VDATA_LINES(mc.lineColor, zTest);
	size_t origNumFaceVerts = vdf.verts.size();
	size_t origNumLineVerts = vdl.verts.size();
	_AddSubdivCube(0, mc, wd, vdf, vdl);

	Vector size, mins;
	matrix3x4_t scaleMat, offMat, finalMat;
	VectorAbs(maxs_ - mins_, size);
	VectorMin(mins_, maxs_, mins); // in case anyone stupid (like me) swaps the mins/maxs
	scaleMat.Init({size.x, 0, 0}, {0, size.y, 0}, {0, 0, size.z}, mins); // set up box dimensions at origin
	AngleMatrix(ang, pos, offMat);                                       // rotate box and put at 'pos'
	MatrixMultiply(offMat, scaleMat, finalMat);

	for (size_t i = origNumFaceVerts; i < vdf.verts.size(); i++)
		utils::VectorTransform(finalMat, vdf.verts[i].pos);
	for (size_t i = origNumLineVerts; i < vdl.verts.size(); i++)
		utils::VectorTransform(finalMat, vdl.verts[i].pos);
}

void MeshBuilderDelegate::AddSphere(const Vector& pos,
                                    float radius,
                                    int nSubdivisions,
                                    MeshColor mc,
                                    bool zTest,
                                    WindingDir wd)
{
	if (nSubdivisions < 0 || radius < 0 || (mc.faceColor.a == 0 && mc.lineColor.a == 0) || !(wd & WD_BOTH))
		return;

	DECLARE_MULTIPLE_COMPONENTS(2);
	auto& vdf = GET_VDATA_FACES(mc.faceColor, zTest);
	auto& vdl = GET_VDATA_LINES(mc.lineColor, zTest);
	size_t origNumFaceVerts = vdf.verts.size();
	size_t origNumLineVerts = vdl.verts.size();
	_AddSubdivCube(nSubdivisions, mc, wd, vdf, vdl);

	// center, normalize, scale, transform :)
	Vector subdivCubeOff((nSubdivisions + 1) / -2.f);
	for (size_t i = origNumFaceVerts; i < vdf.verts.size(); i++)
	{
		Vector& v = vdf.verts[i].pos;
		v += subdivCubeOff;
		VectorNormalize(v);
		v = v * radius + pos;
	}
	for (size_t i = origNumLineVerts; i < vdl.verts.size(); i++)
	{
		Vector& v = vdl.verts[i].pos;
		v += subdivCubeOff;
		VectorNormalize(v);
		v = v * radius + pos;
	}
}

void MeshBuilderDelegate::AddSweptBox(const Vector& start,
                                      const Vector& end,
                                      const Vector& _mins,
                                      const Vector& _maxs,
                                      MeshColor mcStart,
                                      MeshColor mcEnd,
                                      MeshColor mcSweep,
                                      bool zTest,
                                      WindingDir wd)
{
	/*
	* This looks a bit different from the debug overlay swept box, the differences are:
	* - here we actually draw the faces (debug overlay only does wireframe)
	* - we draw the full boxes
	* - we check for axes alignment to make drawings prettier
	* 
	* If you can figure out how this code works on your own then you should probably be doing
	* more important things for humanity, like curing cancer - there's 3 completely different
	* cases and one actually has 3 subcases. A spell can be cast to understand this code, but
	* it requires an ointment that costs 25 gp made from mushroom powder, saffron, and fat.
	*/

	// check for alignment
	Vector diff = end - start;
	bool alignedOn[3];
	int numAlignedAxes = 0;
	for (int i = 0; i < 3; i++)
	{
		alignedOn[i] = fabsf(diff[i]) < 0.01;
		numAlignedAxes += alignedOn[i];
	}

	Vector mins, maxs; // in case anyone stupid (like me) swaps these around
	VectorMin(_mins, _maxs, mins);
	VectorMax(_mins, _maxs, maxs);

	// scale the boxes a bit to prevent z fighting
	const Vector nudge(0.04f);
	AddBox(end, mins - nudge * 1.5, maxs - nudge * 1.5, vec3_angle, mcEnd, zTest, wd);
	AddBox(start, mins - nudge, maxs - nudge, vec3_angle, mcStart, zTest, wd);

	DECLARE_MULTIPLE_COMPONENTS(2);
	auto& vdf = GET_VDATA_FACES(mcSweep.faceColor, zTest);
	auto& vdl = GET_VDATA_LINES(mcSweep.lineColor, zTest);

	// For a given box, we can encode a face of the box with a 0 or 1 corresponding to mins/maxs.
	// With 3 bits, you can encode the two "same" corners on both boxes.
	Vector mm[] = {mins, maxs};

	std::array<Vector, 4> tmpQuad;

	switch (numAlignedAxes)
	{
	case 2:
	{
		int ax0 = alignedOn[0] ? (alignedOn[1] ? 2 : 1) : 0; // extruding along this axis

		if (abs(diff[ax0]) < mm[1][ax0] - mm[0][ax0])
			return; // sweep is shorter than the box size

		int ax1 = (ax0 + 1) % 3;
		int ax2 = (ax0 + 2) % 3;
		/*
		* Connect face A of the start box with face B of the end box. Side by side we have:
		* 1┌───┐2   1┌───┐2
		*  │ A │     │ B │
		* 0└───┘3   0└───┘3
		* We visit verts {(A0, B0, B1, A1), (A1, B1, B2, A2), ...}.
		* Converting each pair of verts in the above list to ax1/ax2 we want:
		* ax1 (pretend this is x in the above diagram): {(0, 0), (0, 1), (1, 1), (1, 0)}
		* ax2 (pretend this is y in the above diagram): {(0, 1), (1, 1), (1, 0), (0, 0)}
		*/
		int ax1Sign = Sign(diff[ax0]) == -1;
		for (int i = 0; i < 16; i++)
		{
			int whichBox = ((i + 1) % 4 / 2 + ax1Sign) % 2;
			const Vector& boxOff = whichBox ? start : end;
			tmpQuad[i % 4][ax0] = boxOff[ax0] + mm[(ax1Sign + whichBox) % 2][ax0];
			tmpQuad[i % 4][ax1] = boxOff[ax1] + mm[(i + 2) % 16 / 8][ax1];
			tmpQuad[i % 4][ax2] = boxOff[ax2] + mm[(i + 6) % 16 / 8][ax2];
			if (i % 4 == 3)
				_AddPolygon(tmpQuad.data(), 4, mcSweep, wd, vdf, vdl);
		}
		break;
	}
	case 1:
	{
		int ax0 = alignedOn[0] ? 0 : (alignedOn[1] ? 1 : 2); // aligned on this axis
		int ax1 = (ax0 + 1) % 3;
		int ax2 = (ax0 + 2) % 3;
		/*
		* Looking at a box edge on, we see two faces like so:
		* A   B   C
		* ┌───┬───┐
		* │   │   │
		* └───┴───┘
		* D   E   F
		* From the perspective of the aligned axis, we wish to connect to 2 boxes like:
		*          C1
		*    ┌──────┐xx
		*    │      │  xxx
		*    │    B1│     xxx
		*  A1└──────┘..      xxx
		*    xx        .        xx
		*      xxx      ..┌──────┐C2
		*         xxx     │b2    │
		*            xxx  │      │
		*               xx└──────┘B2
		*                 A2
		* Where 'x' is shown in the wireframe but '.' is not (only generated for face quads).
		* We'll encode a corner as 3 bits ZYX (e.g. +y is 0b010, -y is 00b00), this allows us
		* to do e.g. 'bits ^ (1 << ax0)' to go to the other corner along ax0. Then we simply
		* index into 'mm' and add start/end to get the final position. In the above diagram
		* ax0 tells us if we're on A,B,C or D,E,F. By starting at corner B, we can flip the
		* bits (1 << ax1) or (1 << ax2) in our encoding scheme to get to corners A or C.
		*/

		size_t origNumFaceIndices = vdf.indices.size();

		// how to get from B2 to b2
		Vector diagOffsetForEndBox = maxs - mins;
		diagOffsetForEndBox[ax0] = 0;
		if (diff[ax1] > 1)
			diagOffsetForEndBox[ax1] *= -1;
		if (diff[ax2] > 1)
			diagOffsetForEndBox[ax2] *= -1;

		int refCBits = (diff.x > 0) | (diff.y > 0) << 1 | (diff.z > 0) << 2; // reference corner bits (corner B)

		int cBits[3] = {refCBits, refCBits ^ (1 << ax1), refCBits ^ (1 << ax2)};
		Vector v1(mm[cBits[0] & 1].x, mm[(cBits[0] & 2) >> 1].y, mm[(cBits[0] & 4) >> 2].z); // B
		Vector v2(mm[cBits[1] & 1].x, mm[(cBits[1] & 2) >> 1].y, mm[(cBits[1] & 4) >> 2].z); // A/C
		Vector v3(mm[cBits[2] & 1].x, mm[(cBits[2] & 2) >> 1].y, mm[(cBits[2] & 4) >> 2].z); // C/A

		Vector size = mm[1] - mm[0];
		Vector ax0Off = size;
		ax0Off[ax1] = ax0Off[ax2] = 0;

		Vector diffAbs;
		VectorAbs(diff, diffAbs);
		Vector overlap = size - diffAbs;

		if (overlap[ax1] > 0 && overlap[ax2] > 0)
		{
			// start/end overlap
			Vector overlapAx1 = overlap;
			Vector overlapAx2 = overlap;
			overlapAx1[ax0] = overlapAx1[ax2] = 0;
			overlapAx2[ax0] = overlapAx2[ax1] = 0;
			// clang-format off
			// TODO convert to _AddTri
			AddTri(start + v2, start + v1 - Sign(diff[ax1]) * overlapAx1, end + v2, mcSweep);
			AddTri(end + v3, start + v1 - Sign(diff[ax2]) * overlapAx2, start + v3, mcSweep);
			AddTri(end + v2 + ax0Off, start + v1 - Sign(diff[ax1]) * overlapAx1 + ax0Off, start + v2 + ax0Off, mcSweep);
			AddTri(start + v3 + ax0Off, start + v1 - Sign(diff[ax2]) * overlapAx2 + ax0Off, end + v3 + ax0Off, mcSweep);
			// clang-format on
		}
		else
		{
			// TODO I need a reserve scope here
			for (int i = 0; i < 2; i++)
			{
				bool doingFaces = i == 0;
				if (doingFaces ? mcSweep.faceColor.a == 0 : mcSweep.lineColor.a == 0)
					continue;

				size_t numVerts = doingFaces ? vdf.verts.size() : vdl.verts.size();
				auto& verts = doingFaces ? vdf.verts : vdl.verts;
				color32 c = doingFaces ? mcSweep.faceColor : mcSweep.lineColor;

				bool mirrorStrips = fabs(diff[ax1]) > fabs(diff[ax2]);
				// e.g. A1 B1 C1
				verts.emplace_back(start + v2, c);
				verts.emplace_back(start + v1, c);
				verts.emplace_back(start + v3, c);
				// e.g. A2 b2 C2
				verts.emplace_back(end + v2, c);
				verts.emplace_back(end + v1 + diagOffsetForEndBox, c);
				verts.emplace_back(end + v3, c);

				if (doingFaces)
				{
					_AddFaceTriangleStripIndices(vdf,
					                             numVerts + 3,
					                             numVerts,
					                             3,
					                             false,
					                             !mirrorStrips,
					                             wd);
				}
				else
				{
					_AddLineStripIndices(vdl, numVerts, 3, false);
					_AddLineStripIndices(vdl, numVerts + 3, 3, false);
				}
				// now do other side of cube by adding ax0Off to all the verts
				for (int j = 0; j < 6; j++)
				{
					VertexData& tmp = verts[numVerts + j];
					verts.emplace_back(tmp.pos + ax0Off, c);
				}
				numVerts += 6;
				if (doingFaces)
				{
					_AddFaceTriangleStripIndices(vdf,
					                             numVerts,
					                             numVerts + 3,
					                             3,
					                             false,
					                             mirrorStrips,
					                             wd);
				}
				else
				{
					_AddLineStripIndices(vdl, numVerts, 3, false);
					_AddLineStripIndices(vdl, numVerts + 3, 3, false);
				}
			}
		}

		tmpQuad = {start + v2 + ax0Off, start + v2, end + v2, end + v2 + ax0Off};
		_AddPolygon(tmpQuad.data(), 4, mcSweep, wd, vdf, vdl);
		tmpQuad = {start + v3, start + v3 + ax0Off, end + v3 + ax0Off, end + v3};
		_AddPolygon(tmpQuad.data(), 4, mcSweep, wd, vdf, vdl);

		if ((wd & WD_BOTH) == WD_BOTH)
		{
			vdf.indices.add_range(std::vector<VertIndex>::reverse_iterator(vdf.indices.begin()
			                                                               + origNumFaceIndices),
			                      std::vector<VertIndex>::reverse_iterator(vdf.indices.end()));
		}
		else if (((diff[ax1] < 0) != (diff[ax2] < 0)) == (bool)(wd & WD_CW))
		{
			// parity
			std::reverse(vdf.indices.begin() + origNumFaceIndices, vdf.indices.end());
		}

		break;
	}
	case 0:
	{
		/*
		* We'll use the same encoding scheme as in case 1. If we look at our box from a corner, we see a hexagonal
		* shape. We encode this corner as refCBits, then go to an adjacent corner cBits1 by flipping a bit, and
		* finally flip another (different) bit to get to cBits2. This gives us a way to fill in a face by using
		* all possible edges cBits1-cBits2 on a single box, (we connect the edge between the boxes to get a quad).
		*/
		int refCBits = (diff.x > 0) | (diff.y > 0) << 1 | (diff.z > 0) << 2; // start corner
		for (int flipBit = 0; flipBit < 3; flipBit++)
		{
			int cBits1 = refCBits ^ (1 << flipBit); // one corner away from ref
			for (int flipBitOff = 0; flipBitOff < 2; flipBitOff++)
			{
				int cBits2 = cBits1 ^ (1 << ((flipBit + flipBitOff + 1) % 3)); // 2 away from ref
				Vector off1(mm[cBits1 & 1].x, mm[(cBits1 & 2) >> 1].y, mm[(cBits1 & 4) >> 2].z);
				Vector off2(mm[cBits2 & 1].x, mm[(cBits2 & 2) >> 1].y, mm[(cBits2 & 4) >> 2].z);
				tmpQuad = {end + off1, end + off2, start + off2, start + off1};
				// parity
				WindingDir wdTmp = ((flipBitOff + (diff.x > 1) + (diff.y > 1) + (diff.z > 1)) % 2)
				                       ? INVERT_WD(wd)
				                       : wd;
				_AddPolygon(tmpQuad.data(), 4, mcSweep, wdTmp, vdf, vdl);
			}
		}
		break;
	}
	}
}

void MeshBuilderDelegate::AddCone(const Vector& pos,
                                  const QAngle& ang,
                                  float height,
                                  float radius,
                                  int nCirclePoints,
                                  bool drawBase,
                                  MeshColor mc,
                                  bool zTest,
                                  WindingDir wd)
{
	if (height < 0 || radius < 0 || nCirclePoints < 3 || (mc.faceColor.a == 0 && mc.lineColor.a == 0)
	    || !(wd & WD_BOTH))
	{
		return;
	}

	Vector* circleVerts = _CreateEllipseVerts(pos, ang, radius, radius, nCirclePoints);

	Vector tip;
	AngleVectors(ang, &tip);
	tip = pos + tip * height;

	if (mc.faceColor.a != 0)
	{
		auto& vdf = GET_VDATA_FACES(mc.faceColor, zTest);
		ReserveScope rs(vdf, nCirclePoints + 1, nCirclePoints * 3 + (drawBase ? 3 * (nCirclePoints - 2) : 0));
		size_t tipIdx = vdf.verts.size();
		vdf.verts.emplace_back(tip, mc.faceColor);
		for (int i = 0; i < nCirclePoints; i++)
		{
			if (i > 0)
			{
				// faces on top of cone
				vdf.indices.push_back(vdf.verts.size() - 1);
				vdf.indices.push_back(vdf.verts.size());
				vdf.indices.push_back(tipIdx);
			}
			vdf.verts.emplace_back(circleVerts[i], mc.faceColor);
		}
		// final face on top of cone
		vdf.indices.push_back(vdf.verts.size() - 1);
		vdf.indices.push_back(tipIdx + 1);
		vdf.indices.push_back(tipIdx);
		if (drawBase)
			_AddFacePolygonIndices(vdf, tipIdx + 1, nCirclePoints, INVERT_WD(wd));
	}

	if (mc.lineColor.a != 0)
	{
		auto& vdl = GET_VDATA_LINES(mc.lineColor, zTest);
		ReserveScope rs(vdl, nCirclePoints + 1, nCirclePoints * 4);
		size_t tipIdx = vdl.verts.size();
		vdl.verts.emplace_back(tip, mc.lineColor);
		AddLineStrip(circleVerts, nCirclePoints, true, mc.lineColor);
		for (int i = 0; i < nCirclePoints; i++)
		{
			// lines from tip to base
			vdl.indices.push_back(tipIdx);
			vdl.indices.push_back(tipIdx + i + 1);
		}
	}
}

void MeshBuilderDelegate::AddCylinder(const Vector& pos,
                                      const QAngle& ang,
                                      float height,
                                      float radius,
                                      int nCirclePoints,
                                      bool drawCap1,
                                      bool drawCap2,
                                      MeshColor mc,
                                      bool zTest,
                                      WindingDir wd)
{
	if (nCirclePoints < 3 || height < 0 || (mc.faceColor.a == 0 && mc.lineColor.a == 0) || !(wd & WD_BOTH))
		return;

	Vector heightOff;
	AngleVectors(ang, &heightOff);
	heightOff *= height;

	Vector* circleVerts = _CreateEllipseVerts(pos, ang, radius, radius, nCirclePoints);

	if (mc.faceColor.a != 0)
	{
		auto& vdf = GET_VDATA_FACES(mc.faceColor, zTest);
		ReserveScope rs(vdf,
		                nCirclePoints * 2,
		                (nCirclePoints * 6 + (nCirclePoints - 2) * 3 * (drawCap1 + drawCap2))
		                    * INDEX_COUNT_MULTIPLIER(wd));
		size_t idx1 = vdf.verts.size();
		for (int i = 0; i < nCirclePoints; i++)
			vdf.verts.emplace_back(circleVerts[i], mc.faceColor);
		size_t idx2 = vdf.verts.size();
		for (int i = 0; i < nCirclePoints; i++)
			vdf.verts.emplace_back(circleVerts[i] + heightOff, mc.faceColor);
		_AddFaceTriangleStripIndices(vdf, idx2, idx1, nCirclePoints, true, false, wd);
		if (drawCap1)
			_AddFacePolygonIndices(vdf, idx1, nCirclePoints, INVERT_WD(wd));
		if (drawCap2)
			_AddFacePolygonIndices(vdf, idx2, nCirclePoints, wd);
	}

	if (mc.lineColor.a != 0)
	{
		auto& vdl = GET_VDATA_LINES(mc.lineColor, zTest);
		ReserveScope rs(vdl, nCirclePoints * 2, nCirclePoints * 6);
		size_t idx1 = vdl.verts.size();
		for (int i = 0; i < nCirclePoints; i++)
			vdl.verts.emplace_back(circleVerts[i], mc.lineColor);
		size_t idx2 = vdl.verts.size();
		for (int i = 0; i < nCirclePoints; i++)
			vdl.verts.emplace_back(circleVerts[i] + heightOff, mc.lineColor);
		_AddLineStripIndices(vdl, idx1, nCirclePoints, true);
		_AddLineStripIndices(vdl, idx2, nCirclePoints, true);
		for (int i = 0; i < nCirclePoints; i++)
		{
			vdl.indices.push_back(idx1 + i);
			vdl.indices.push_back(idx2 + i);
		}
	}
}

void MeshBuilderDelegate::AddArrow3D(const Vector& pos,
                                     const Vector& target,
                                     float tailLength,
                                     float tailRadius,
                                     float tipHeight,
                                     float tipRadius,
                                     int nCirclePoints,
                                     MeshColor mc,
                                     bool zTest,
                                     WindingDir wd)
{
	if (nCirclePoints < 3 || (mc.faceColor.a == 0 && mc.lineColor.a == 0))
		return;

	DECLARE_MULTIPLE_COMPONENTS(2);
	auto& vdf = GET_VDATA_FACES(mc.faceColor, zTest);
	auto& vdl = GET_VDATA_LINES(mc.lineColor, zTest);

	Vector dir = target - pos;
	dir.NormalizeInPlace();
	QAngle ang;
	VectorAngles(dir, ang);

	AddCylinder(pos, ang, tailLength, tailRadius, nCirclePoints, true, false, mc, zTest, wd);
	// assumes AddCylinder() puts the "top" vertices at the end of the vert list for faces & lines
	size_t innerCircleFaceIdx = vdf.verts.size() - nCirclePoints;
	size_t innerCircleLineIdx = vdl.verts.size() - nCirclePoints;
	AddCone(pos + dir * tailLength, ang, tipHeight, tipRadius, nCirclePoints, false, mc, zTest, wd);
	// assumes AddCone() puts the base vertices at the end of the vert list for faces & lines
	size_t outerCircleFaceIdx = vdf.verts.size() - nCirclePoints;
	size_t outerCircleLineIdx = vdl.verts.size() - nCirclePoints;

	// TODO TEST MIRROR WITH LOOP HERE
	if (mc.faceColor.a != 0)
		_AddFaceTriangleStripIndices(vdf,
		                             outerCircleFaceIdx,
		                             innerCircleFaceIdx,
		                             nCirclePoints,
		                             true,
		                             false,
		                             wd);

	if (mc.lineColor.a != 0)
	{
		ReserveScope rs(vdl, 0, nCirclePoints * 2);
		for (int i = 0; i < nCirclePoints; i++)
		{
			vdl.indices.push_back(innerCircleLineIdx + i);
			vdl.indices.push_back(outerCircleLineIdx + i);
		}
	}
}

void MeshBuilderDelegate::AddCPolyhedron(const CPolyhedron* polyhedron, MeshColor mc, bool zTest, WindingDir wd)
{
	if (!polyhedron || !(wd & WD_BOTH))
		return;
	if (mc.faceColor.a != 0)
	{
		auto& vdf = GET_VDATA_FACES(mc.faceColor, zTest);
		size_t totalIndices = 0;
		for (int p = 0; p < polyhedron->iPolygonCount; p++)
			totalIndices += (polyhedron->pPolygons[p].iIndexCount - 2) * 3;
		ReserveScope rs(vdf, polyhedron->iVertexCount, totalIndices * INDEX_COUNT_MULTIPLIER(wd));
		const size_t initIdx = vdf.verts.size();
		for (int v = 0; v < polyhedron->iVertexCount; v++)
			vdf.verts.emplace_back(polyhedron->pVertices[v], mc.faceColor);
		for (int p = 0; p < polyhedron->iPolygonCount; p++)
		{
			// Writing the indices straight from the polyhedron data instead of converting to polygons first.
			// To make a tri we do the same thing as in AddPolygon - fix a vert and iterate over the rest.

			const Polyhedron_IndexedPolygon_t& polygon = polyhedron->pPolygons[p];
			unsigned short firstIdx = 0, prevIdx = 0;

			for (int i = 0; i < polygon.iIndexCount; i++)
			{
				Polyhedron_IndexedLineReference_t& ref = polyhedron->pIndices[polygon.iFirstIndex + i];
				Polyhedron_IndexedLine_t& line = polyhedron->pLines[ref.iLineIndex];
				unsigned short curIdx = initIdx + line.iPointIndices[ref.iEndPointIndex];
				switch (i)
				{
				case 0:
					firstIdx = curIdx;
				case 1:
					break;
				default:
					if (wd & WD_CW)
					{
						vdf.indices.push_back(firstIdx);
						vdf.indices.push_back(prevIdx);
						vdf.indices.push_back(curIdx);
					}
					if (wd & WD_CCW)
					{
						vdf.indices.push_back(curIdx);
						vdf.indices.push_back(prevIdx);
						vdf.indices.push_back(firstIdx);
					}
					break;
				}
				prevIdx = curIdx;
			}
		}
	}
	if (mc.lineColor.a != 0)
	{
		// edges are more straightforward since the polyhedron stores edges as pairs of indices already
		auto& vdl = GET_VDATA_LINES(mc.lineColor, zTest);
		ReserveScope rs(vdl, polyhedron->iVertexCount, polyhedron->iLineCount * 2);
		const size_t initIdx = vdl.verts.size();
		for (int v = 0; v < polyhedron->iVertexCount; v++)
			vdl.verts.emplace_back(polyhedron->pVertices[v], mc.lineColor);
		for (int i = 0; i < polyhedron->iLineCount; i++)
		{
			Polyhedron_IndexedLine_t& line = polyhedron->pLines[i];
			vdl.indices.push_back(initIdx + line.iPointIndices[0]);
			vdl.indices.push_back(initIdx + line.iPointIndices[1]);
		}
	}
}

void MeshBuilderDelegate::_AddFaceTriangleStripIndices(MeshVertData& vdf,
                                                       size_t vIdx1,
                                                       size_t vIdx2,
                                                       size_t nVerts,
                                                       bool loop,
                                                       bool mirror,
                                                       WindingDir wd)
{
	/*
	* Creates indices representing a filled triangle strip using existing verts at the given vert indices for faces only.
	* +---+---+---+  <- verts at vIdx1
	* |\  |\  |\  |
	* | \ | \ | \ |
	* |  \|  \|  \|
	* +---+---+---+  <- verts at vIdx2
	* If mirroring, the triangles lean the other way (but still face the same direction), only used for swept boxes.
	*/
	Assert(wd & WD_BOTH);
	if (loop)
		Assert(nVerts >= 3);
	else
		Assert(nVerts >= 2);

	ReserveScope rs(vdf, 0, (nVerts - 1 + loop) * 6 * INDEX_COUNT_MULTIPLIER(wd));
	size_t origSize = vdf.indices.size();

	for (size_t i = 0; i < nVerts - 1; i++)
	{
		if (mirror)
		{
			vdf.indices.push_back(vIdx1 + i);
			vdf.indices.push_back(vIdx2 + i);
			vdf.indices.push_back(vIdx1 + i + 1);
			vdf.indices.push_back(vIdx2 + i);
			vdf.indices.push_back(vIdx2 + i + 1);
			vdf.indices.push_back(vIdx1 + i + 1);
		}
		else
		{
			vdf.indices.push_back(vIdx1 + i);
			vdf.indices.push_back(vIdx2 + i);
			vdf.indices.push_back(vIdx2 + i + 1);
			vdf.indices.push_back(vIdx2 + i + 1);
			vdf.indices.push_back(vIdx1 + i + 1);
			vdf.indices.push_back(vIdx1 + i);
		}
	}
	if (loop)
	{
		if (mirror) // loop + mirror not tested
		{
			vdf.indices.push_back(vIdx1 + nVerts - 1);
			vdf.indices.push_back(vIdx2 + nVerts - 1);
			vdf.indices.push_back(vIdx1);
			vdf.indices.push_back(vIdx2 + nVerts - 1);
			vdf.indices.push_back(vIdx2);
			vdf.indices.push_back(vIdx1);
		}
		else
		{
			vdf.indices.push_back(vIdx1 + nVerts - 1);
			vdf.indices.push_back(vIdx2 + nVerts - 1);
			vdf.indices.push_back(vIdx2);
			vdf.indices.push_back(vIdx2);
			vdf.indices.push_back(vIdx1);
			vdf.indices.push_back(vIdx1 + nVerts - 1);
		}
	}

	if ((wd & WD_BOTH) == WD_BOTH)
	{
		vdf.indices.add_range(std::vector<VertIndex>::reverse_iterator(vdf.indices.end()),
		                      std::vector<VertIndex>::reverse_iterator(vdf.indices.begin() + origSize));
	}
	else if ((wd & WD_BOTH) == WD_CCW)
	{
		std::reverse(vdf.indices.begin() + origSize, vdf.indices.end());
	}
	else
	{
		return;
	}
}

void MeshBuilderDelegate::_AddFacePolygonIndices(MeshVertData& vdf, size_t vertsIdx, int nVerts, WindingDir wd)
{
	// Creates indices representing a filled convex polygon using existing verts at the given vert index for faces only.
	Assert(nVerts >= 3);
	Assert(wd & WD_BOTH);
	ReserveScope rs(vdf, 0, (nVerts - 2) * 3 * INDEX_COUNT_MULTIPLIER(wd));
	for (int i = 0; i < nVerts - 2; i++)
	{
		if (wd & WD_CW)
		{
			vdf.indices.push_back(vertsIdx);
			vdf.indices.push_back(vertsIdx + i + 1);
			vdf.indices.push_back(vertsIdx + i + 2);
		}
		if (wd & WD_CCW)
		{
			vdf.indices.push_back(vertsIdx + i + 2);
			vdf.indices.push_back(vertsIdx + i + 1);
			vdf.indices.push_back(vertsIdx);
		}
	}
}

void MeshBuilderDelegate::_AddLineStripIndices(MeshVertData& vdl, size_t vertsIdx, int nVerts, bool loop)
{
	Assert(nVerts >= 2);
	ReserveScope rs(vdl, 0, (nVerts - 1 + (loop && nVerts > 2)) * 2);
	for (int i = 0; i < nVerts - 1; i++)
	{
		vdl.indices.push_back(vertsIdx + i);
		vdl.indices.push_back(vertsIdx + i + 1);
	}
	if (loop && nVerts > 2)
	{
		vdl.indices.push_back(vertsIdx + nVerts - 1);
		vdl.indices.push_back(vertsIdx);
	}
}

void MeshBuilderDelegate::_AddSubdivCube(int nSubdivisions,
                                         MeshColor mc,
                                         WindingDir wd,
                                         MeshVertData& vdf,
                                         MeshVertData& vdl)
{
	bool doFaces = mc.faceColor.a != 0;
	bool doLines = mc.lineColor.a != 0;
	if (nSubdivisions < 0 || (!doFaces && !doLines))
		return;

	/*
	* Each cube face will look like this for e.g. 2 subdivisions:
	* ┌───┬───┬───┐
	* │   │   │   │
	* ├───┼───┼───┤
	* │   │   │   │
	* ├───┼───┼───┤
	* │   │   │   │
	* └───┴───┴───┘
	* Mins are at <0,0,0> & maxs are at <subdivs + 1, subdivs + 1, subdivs + 1>.
	* I used to make this out of quads but the current alg uses 1.5-4 times less verts depending on nSubdivisions.
	*/

	int sideLength = nSubdivisions + 1;
	int nLayerVerts = sideLength * 4;

	int numTotalVerts = 2 * (3 * sideLength + 1) * (sideLength + 1);
	int numTotalFaceIndices = 36 * sideLength * sideLength * INDEX_COUNT_MULTIPLIER(wd);
	int numTotalLineIndices = 12 * sideLength * (2 * sideLength + 1);
	ReserveScope rsf(vdf, doFaces ? numTotalVerts : 0, doFaces ? numTotalFaceIndices : 0);
	ReserveScope rsl(vdl, doLines ? numTotalVerts : 0, doLines ? numTotalLineIndices : 0);

	// fill in everything except for caps by winding around the cube
	for (int z = 0; z <= sideLength; z++)
	{
		int x = 0;
		int y = 0;
		for (int i = 0; i < nLayerVerts; i++)
		{
			// add all verts for this layer in a counter-clockwise order
			if (i < sideLength)
				x++;
			else if (i < sideLength * 2)
				y++;
			else if (i < sideLength * 3)
				x--;
			else
				y--;

			if (doFaces)
				vdf.verts.push_back({Vector(x, y, z), mc.faceColor});
			if (doLines)
				vdl.verts.push_back({Vector(x, y, z), mc.lineColor});
		}
		if (doFaces && z > 0)
		{
			size_t n = vdf.verts.size();
			_AddFaceTriangleStripIndices(vdf,
			                             n - nLayerVerts * 2,
			                             n - nLayerVerts,
			                             nLayerVerts,
			                             true,
			                             false,
			                             wd);
		}
		if (doLines)
		{
			size_t n = vdl.verts.size();
			_AddLineStripIndices(vdl, n - nLayerVerts, nLayerVerts, true);
			if (z > 0)
			{
				for (int k = 0; k < nLayerVerts; k++)
				{
					vdl.indices.push_back(n + k - nLayerVerts * 2);
					vdl.indices.push_back(n + k - nLayerVerts);
				}
			}
		}
	}

	// TODO add a special case here for no subdivisions

	// fill caps
	for (int top = 0; top <= 1; top++)
	{
		int z = top * sideLength;
		for (int x = 0; x <= sideLength; x++)
		{
			for (int y = 0; y <= sideLength; y++)
			{
				if (doFaces)
					vdf.verts.push_back({Vector(x, y, z), mc.faceColor});
				if (doLines)
					vdl.verts.push_back({Vector(x, y, z), mc.lineColor});
			}
			if (doFaces && x > 0)
			{
				size_t a = vdf.verts.size() - (sideLength + 1) * 2;
				size_t b = vdf.verts.size() - (sideLength + 1);
				if (top)
					std::swap(a, b);
				_AddFaceTriangleStripIndices(vdf, a, b, sideLength + 1, false, false, wd);
			}
			if (doLines)
			{
				size_t n = vdl.verts.size();
				_AddLineStripIndices(vdl, n - 1 - sideLength, sideLength + 1, false);
				if (x > 0)
				{
					for (int k = 0; k < sideLength; k++)
					{
						vdl.indices.push_back(n + k - (sideLength + 1) * 2);
						vdl.indices.push_back(n + k - (sideLength + 1));
					}
				}
			}
		}
	}
}
Vector* MeshBuilderDelegate::_CreateEllipseVerts(const Vector& pos,
                                                 const QAngle& ang,
                                                 float radiusA,
                                                 float radiusB,
                                                 int nPoints)
{
	VMatrix mat;
	mat.SetupMatrixOrgAngles(pos, ang);
	Vector* scratch = Scratch(nPoints);
	for (int i = 0; i < nPoints; i++)
	{
		float s, c;
		SinCos(M_PI_F * 2 / nPoints * i, &s, &c);
		// oriented clockwise, normal is towards x+ for an angle of <0,0,0>
		scratch[i] = mat * Vector(0, s * radiusA, c * radiusB);
	}
	return scratch;
}

#endif
