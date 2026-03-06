#include "stdafx.hpp"

#include "tr_imgui.hpp"

#ifdef SPT_PLAYER_TRACE_ENABLED

#include "spt/utils/game_detection.hpp"
#include "spt/features/visualizations/imgui/imgui_interface.hpp"

using namespace player_trace;

void tr_imgui::SingleTraceInfoTabHeader(tr_tick activeTick)
{
	auto& tr = TrReadContextScope::Current();

	if (tr.hasStartRecordingBeenCalled)
	{
		ImGui::Text("Active tick: %u/%u", activeTick, tr.numRecordedTicks == 0 ? 0 : tr.numRecordedTicks - 1);
	}
	else
	{
		ImGui::TextColored(SPT_IMGUI_WARN_COLOR_YELLOW,
		                   "%s",
		                   ICON_CI_WARNING " No active trace " ICON_CI_WARNING);
	}
}

void tr_imgui::PlayerTabCallback(tr_tick activeTick)
{
	auto& tr = TrReadContextScope::Current();

	tr_struct_version playerExportVersion = tr.GetFirstExportVersion<TrPlayerData>();
	auto plIdx = tr.GetAtTick<TrPlayerData>(activeTick);
	TrPlayerData defaultPl{};
	auto& pl = plIdx.IsValid() ? **plIdx : defaultPl;
	Vector vecInvalid{NAN, NAN, NAN};
	QAngle angInvalid{NAN, NAN, NAN};
	TrTransform transInvalid{};

	Vector eyePos, sgEyePos, vPhysPos;
	QAngle eyeAng, sgEyeAng, vPhysAng;
	pl.transVPhysIdx.GetOrDefault(transInvalid).GetPosAng(vPhysPos, vPhysAng);
	pl.transEyesIdx.GetOrDefault(transInvalid).GetPosAng(eyePos, eyeAng);
	pl.transSgEyesIdx.GetOrDefault(transInvalid).GetPosAng(sgEyePos, sgEyeAng);

	ImGui::Text("QPhys pos: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(pl.qPosIdx.GetOrDefault(vecInvalid)));
	ImGui::Text("VPhys pos: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(vPhysPos));
	ImGui::Text("VPhys ang: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(vPhysAng));
	ImGui::Text("QPhys vel: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(pl.qVelIdx.GetOrDefault(vecInvalid)));
	if (playerExportVersion >= 2)
		ImGui::Text("VPhys vel: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(pl.vVelIdx.GetOrDefault(vecInvalid)));
	else
		ImGui::TextDisabled("VPhys vel: data was not recorded on this trace version");
	ImGui::Text("Eye pos: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(eyePos));
	ImGui::Text("Eye ang: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(eyeAng));
	if (utils::DoesGameLookLikePortal())
	{
		if (playerExportVersion >= 2)
		{
			if (pl.envPortalHandle.IsValid())
			{
				ImGui::Text("Portal environment: %d, (serial: %d)",
				            pl.envPortalHandle.GetEntryIndex(),
				            pl.envPortalHandle.GetSerialNumber());
			}
			else
			{
				ImGui::TextDisabled("Portal environment: NULL");
			}
		}
		else
		{
			ImGui::TextDisabled("Portal environment: data was not recorded on this trace version");
		}
		if (eyePos == sgEyePos)
		{
			ImGui::TextDisabled("SG eye pos: same as eye pos");
			ImGui::TextDisabled("SG eye ang: same as eye ang");
		}
		else
		{
			ImGui::Text("SG eye pos: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(sgEyePos));
			ImGui::Text("SG eye ang: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(sgEyeAng));
		}
	}
	ImGui::Text("m_fFlags: %d", pl.m_fFlags);
	ImGui::Text("fov: %u", pl.fov);
	ImGui::Text("health: %u", pl.m_iHealth);
	ImGui::Text("life state: %u", pl.m_lifeState);
	ImGui::Text("collision group: %u", pl.m_CollisionGroup);
	ImGui::Text("move type: %u", pl.m_MoveType);

	auto contactSp = *pl.contactPtsSp;

	ImGui::Text("%u contact point%s%s",
	            contactSp.size(),
	            contactSp.size() == 1 ? "" : "s",
	            contactSp.empty() ? "" : ":");

	ImGui::Indent();

	for (auto contactIdx : contactSp)
	{
		const TrPlayerContactPoint& pcp = contactIdx.GetOrDefault();
		if (SptImGui::BeginBordered())
		{
			ImGui::PushID((int)contactIdx);
			ImGui::Text("object: '%s' (player is object %d)", *pcp.objNameIdx, !pcp.playerIsObj0);
			ImGui::Text("pos: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(pcp.posIdx.GetOrDefault(vecInvalid)));
			ImGui::Text("normal: " TR_IMG_VEC_FMT, TR_IMG_VEC_UNP(pcp.normIdx.GetOrDefault(vecInvalid)));
			ImGui::Text("force: %f", pcp.force);
			ImGui::PopID();
			SptImGui::EndBordered();
		}
	}

	ImGui::Unindent();
}

#endif
