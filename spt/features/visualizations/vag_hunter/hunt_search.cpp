#include "stdafx.hpp"
#include "hunt.hpp"

Vector HtPortalPair::CalcVagPt(bool blueEntry) const
{
	const HtPortal& entry = blueEntry ? blue : orange;
	const HtPortal& exit = blueEntry ? orange : blue;
	Vector vagPt = entry.pos;
	matrix3x4_t mat;
	AngleIMatrix(QAngle{entry.pitch, entry.yaw, 0.f}, entry.pos, mat);
	utils::VectorTransform(mat, vagPt);
	vagPt[0] = -vagPt[0];
	vagPt[1] = -vagPt[1];
	AngleMatrix(QAngle{exit.pitch, exit.yaw, 0.f}, exit.pos, mat);
	utils::VectorTransform(mat, vagPt);
	return vagPt;
}
