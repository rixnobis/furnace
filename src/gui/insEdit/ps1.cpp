/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "insEditCommon.h"
#include "../intConst.h"

#define PS1_RATE_TOOLTIP \
  if (ImGui::IsItemHovered()) { \
    ImGui::SetTooltip("%s",_("hardware register value: lower = faster")); \
  }

void FurnaceGUI::insEditPS1(DivInstrument* ins) {
  std::vector<FurnaceGUIMacroDesc> macroList;

  insTabSample(ins);

  if (ImGui::BeginTabItem("PlayStation")) {
    ImVec2 sliderSize=ImVec2(20.0f*dpiScale,128.0*dpiScale);
    if (ImGui::BeginTable("PS1EnvParams",5,ImGuiTableFlags_NoHostExtendX)) {
      ImGui::TableSetupColumn("c0",ImGuiTableColumnFlags_WidthFixed,sliderSize.x);
      ImGui::TableSetupColumn("c1",ImGuiTableColumnFlags_WidthFixed,sliderSize.x);
      ImGui::TableSetupColumn("c2",ImGuiTableColumnFlags_WidthFixed,sliderSize.x);
      ImGui::TableSetupColumn("c3",ImGuiTableColumnFlags_WidthFixed,sliderSize.x);
      ImGui::TableSetupColumn("c4",ImGuiTableColumnFlags_WidthFixed,sliderSize.x);

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      CENTER_TEXT("A");
      ImGui::TextUnformatted("A");
      ImGui::TableNextColumn();
      CENTER_TEXT("D");
      ImGui::TextUnformatted("D");
      ImGui::TableNextColumn();
      CENTER_TEXT("SL");
      ImGui::TextUnformatted("SL");
      ImGui::TableNextColumn();
      CENTER_TEXT("SR");
      ImGui::TextUnformatted("SR");
      ImGui::TableNextColumn();
      CENTER_TEXT("R");
      ImGui::TextUnformatted("R");

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      P(CWVSliderScalar("##Attack",sliderSize,ImGuiDataType_U8,&ins->ps1.ar,&_ZERO,&_ONE_HUNDRED_TWENTY_SEVEN)); rightClickable
      PS1_RATE_TOOLTIP
      ImGui::TableNextColumn();
      P(CWVSliderScalar("##Decay",sliderSize,ImGuiDataType_U8,&ins->ps1.dr,&_ZERO,&_FIFTEEN)); rightClickable
      PS1_RATE_TOOLTIP
      ImGui::TableNextColumn();
      P(CWVSliderScalar("##SusLevel",sliderSize,ImGuiDataType_U8,&ins->ps1.sl,&_ZERO,&_FIFTEEN)); rightClickable
      ImGui::TableNextColumn();
      P(CWVSliderScalar("##SusRate",sliderSize,ImGuiDataType_U8,&ins->ps1.sr,&_ZERO,&_ONE_HUNDRED_TWENTY_SEVEN)); rightClickable
      PS1_RATE_TOOLTIP
      ImGui::TableNextColumn();
      P(CWVSliderScalar("##Release",sliderSize,ImGuiDataType_U8,&ins->ps1.rr,&_ZERO,&_THIRTY_ONE)); rightClickable
      PS1_RATE_TOOLTIP

      ImGui::EndTable();
    }

    P(ImGui::Checkbox(_("Exponential attack"),&ins->ps1.arExp));
    ImGui::Text(_("Sustain direction:"));
    ImGui::SameLine();
    if (ImGui::RadioButton(_("Increase"),!ins->ps1.srDec)) { PARAMETER
      ins->ps1.srDec=false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(_("Decrease"),ins->ps1.srDec)) { PARAMETER
      ins->ps1.srDec=true;
    }
    P(ImGui::Checkbox(_("Exponential sustain"),&ins->ps1.srExp));
    P(ImGui::Checkbox(_("Exponential release"),&ins->ps1.rrExp));

    ImGui::Separator();
    P(ImGui::Checkbox(_("Reverb"),&ins->ps1.reverb));
    P(ImGui::Checkbox(_("Noise"),&ins->ps1.noise));
    P(ImGui::Checkbox(_("Pitch mod"),&ins->ps1.pitchMod));

    ImGui::Text("ADSR: %.4X %.4X",ins->ps1.getADSR1(),ins->ps1.getADSR2());
    ImGui::EndTabItem();
  }

  if (ImGui::BeginTabItem(_("Macros"))) {
    macroList.push_back(FurnaceGUIMacroDesc(_("Volume"),&ins->std.volMacro,0,127,160,uiColors[GUI_COLOR_MACRO_VOLUME]));
    macroList.push_back(FurnaceGUIMacroDesc(_("Arpeggio"),&ins->std.arpMacro,-120,120,160,uiColors[GUI_COLOR_MACRO_PITCH],true,NULL,macroHoverNote,false,NULL,true,ins->std.arpMacro.val));
    macroList.push_back(FurnaceGUIMacroDesc(_("Pitch"),&ins->std.pitchMacro,-2048,2047,160,uiColors[GUI_COLOR_MACRO_PITCH],true,macroRelativeMode));

    drawMacros(macroList,macroEditStateMacros,ins);
    ImGui::EndTabItem();
  }
}
