/*
 * Реализация C-шима над Dear ImGui. См. aurora_imgui.h.
 *
 * Рисуем через foreground draw list (без окон imgui): UI у нас — плоский
 * оверлей, ввод обрабатывает cl_touch.c, imgui только рисует.
 *
 * Тема — по gameport/docs/imgui-theme-spec.md (палитра + базовая сетка
 * метрик с масштабом от размера шрифта). Оверлей лежит поверх игровой
 * картинки, поэтому цвета спеки используются с полупрозрачной альфой.
 */

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "aurora_imgui.h"

#include <stdio.h>
#include <stdint.h>

/* Noto Sans Bold (кириллица), сжатый stb-массив — генерируется
   binary_to_compressed_c из imgui/fonts/NotoSans-Bold.ttf. */
#include "../imgui/fonts/NotoSansBold.h"

/* Палитра темы (imgui-theme-spec.md, секция 2). */
#define THEME_HEX(h) ImVec4(((h >> 16) & 0xFF) / 255.f, ((h >> 8) & 0xFF) / 255.f, (h & 0xFF) / 255.f, 1.0f)
namespace Theme
{
	static const ImVec4 BgWindow      = THEME_HEX(0x1F1F22);
	static const ImVec4 Frame         = THEME_HEX(0x2A2A2F);
	static const ImVec4 FrameHover    = THEME_HEX(0x34343A);
	static const ImVec4 FrameActive   = THEME_HEX(0x3D3D44);
	static const ImVec4 ButtonActive  = THEME_HEX(0x4A4A52);
	static const ImVec4 Border        = THEME_HEX(0x0B0B0D);
	static const ImVec4 Text          = THEME_HEX(0xE6E6EA);
	static const ImVec4 Accent        = THEME_HEX(0x3B82F6);
	static const ImVec4 AccentLine    = THEME_HEX(0x5AA0FF);
}

static bool l_ready = false;

static ImU32 ThemeColor(ImVec4 c, float alpha)
{
	return IM_COL32((int)(c.x * 255.f), (int)(c.y * 255.f), (int)(c.z * 255.f),
		(int)(alpha * 255.f));
}

/* Тема по imgui-theme-spec.md: базовая сетка метрик (под шрифт 13 px)
   + масштаб всей сетки от фактического размера шрифта.
   Нестатическая: используется и лаунчером (aurora_launcher.cpp). */
void AuroraImgui_ApplyTheme(float fontSizePx)
{
	ImGui::StyleColorsDark();

	ImGuiStyle &s = ImGui::GetStyle();
	s.WindowPadding      = ImVec2(10, 10);
	s.FramePadding       = ImVec2(6, 3);
	s.ItemSpacing        = ImVec2(6, 4);
	s.ItemInnerSpacing   = ImVec2(4, 4);
	s.IndentSpacing      = 12.0f;
	s.WindowBorderSize   = 1.0f;
	s.FrameBorderSize    = 1.0f;
	s.WindowRounding     = 0.0f;
	s.FrameRounding      = 2.0f;
	s.GrabRounding       = 1.0f;
	s.ScrollbarSize      = 10.0f;
	s.TabRounding        = 0.0f;
	s.ScaleAllSizes(fontSizePx / 13.0f);

	/* Фирменное скругление кнопок порта: ~30% высоты кнопки при высоте
	   кнопки ~3.3 font. Сознательное отступление от спеки (там 2 px);
	   ставим ПОСЛЕ ScaleAllSizes, чтобы значение не масштабировалось. */
	s.FrameRounding      = fontSizePx;

	ImVec4 *c = s.Colors;
	c[ImGuiCol_WindowBg]         = Theme::BgWindow;
	c[ImGuiCol_ChildBg]          = THEME_HEX(0x16161A);
	c[ImGuiCol_PopupBg]          = THEME_HEX(0x232328);
	c[ImGuiCol_FrameBg]          = Theme::Frame;
	c[ImGuiCol_FrameBgHovered]   = Theme::FrameHover;
	c[ImGuiCol_FrameBgActive]    = Theme::FrameActive;
	c[ImGuiCol_Button]           = Theme::FrameHover;
	c[ImGuiCol_ButtonHovered]    = THEME_HEX(0x3F3F46);
	c[ImGuiCol_ButtonActive]     = Theme::ButtonActive;
	c[ImGuiCol_Header]           = THEME_HEX(0x2E2E34);
	c[ImGuiCol_HeaderHovered]    = THEME_HEX(0x383840);
	c[ImGuiCol_HeaderActive]     = THEME_HEX(0x424249);
	c[ImGuiCol_Border]           = Theme::Border;
	c[ImGuiCol_Separator]        = THEME_HEX(0x2E2E33);
	c[ImGuiCol_Text]             = Theme::Text;
	c[ImGuiCol_TextDisabled]     = THEME_HEX(0x5D5D66);
	c[ImGuiCol_CheckMark]        = THEME_HEX(0xFFFFFF);
	c[ImGuiCol_SliderGrab]       = Theme::Accent;
	c[ImGuiCol_SliderGrabActive] = Theme::AccentLine;
}

bool AuroraImgui_Init(float fontSizePx)
{
	if (l_ready)
		return true;

	/* Спека: кламп размера шрифта 14..96 px (битый DPI не должен дать
	   микроскопический или гигантский шрифт). */
	if (fontSizePx < 14.0f)
		fontSizePx = 14.0f;
	if (fontSizePx > 96.0f)
		fontSizePx = 96.0f;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = NULL; /* imgui.ini не пишем (песочница) */
	io.LogFilename = NULL;

	AuroraImgui_ApplyTheme(fontSizePx);

	/* Шрифт — Noto Sans Bold, размер от DPI (~3 мм, кламп выше). Данные
	   проверены хост-тестом (атлас, 624 глифа). Диапазоны — явно:
	   Latin-1 + Cyrillic + General Punctuation (тире 0x2014 и пр. — в
	   GetGlyphRangesCyrillic их нет). */
	static const ImWchar kGlyphRanges[] = {
		0x0020, 0x00FF, /* Latin-1 + пунктуация */
		0x0400, 0x04FF, /* Cyrillic */
		0x2010, 0x205E, /* General Punctuation (— „ “ ” …) */
		0,
	};
	ImFont *font = io.Fonts->AddFontFromMemoryCompressedTTF(
		NotoSansBold_compressed_data, NotoSansBold_compressed_size,
		fontSizePx, NULL, kGlyphRanges);
	if (font == NULL)
	{
		ImGui::DestroyContext();
		return false;
	}

	if (!ImGui_ImplOpenGL3_Init())
	{
		ImGui::DestroyContext();
		return false;
	}

	l_ready = true;
	printf("AuroraImgui: init ok, font %.0fpx (Noto Sans Bold)\n", fontSizePx);
	fflush(stdout);
	return true;
}

void AuroraImgui_Shutdown(void)
{
	if (!l_ready)
		return;
	ImGui_ImplOpenGL3_Shutdown();
	ImGui::DestroyContext();
	l_ready = false;
}

bool AuroraImgui_IsReady(void)
{
	return l_ready;
}

void AuroraImgui_NewFrame(int width, int height)
{
	if (!l_ready)
		return;

	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2((float)width, (float)height);
	io.DeltaTime = 1.0f / 60.0f;

	ImGui_ImplOpenGL3_NewFrame();
	ImGui::NewFrame();

	/* Однократная диагностика: атлас и GL-текстура шрифта. */
	static bool l_logged = false;
	if (!l_logged)
	{
		l_logged = true;
		int aw = 0, ah = 0;
		unsigned char *px = NULL;
		io.Fonts->GetTexDataAsAlpha8(&px, &aw, &ah);
		printf("AuroraImgui: atlas %dx%d built=%d TexID=%u\n", aw, ah,
			(int)io.Fonts->IsBuilt(), (unsigned)(uintptr_t)io.Fonts->TexID);
		fflush(stdout);
	}
}

void AuroraImgui_Render(void)
{
	if (!l_ready)
		return;

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void AuroraImgui_Panel(float x, float y, float w, float h)
{
	if (!l_ready)
		return;

	ImGuiStyle &style = ImGui::GetStyle();
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	ImVec2 p0(x, y), p1(x + w, y + h);
	dl->AddRectFilled(p0, p1, ThemeColor(Theme::BgWindow, 0.45f), style.WindowRounding);
	dl->AddRect(p0, p1, ThemeColor(Theme::Border, 0.55f), style.WindowRounding,
		0, style.WindowBorderSize);
}

void AuroraImgui_Button(float x, float y, float w, float h, const char *label, bool pressed)
{
	if (!l_ready)
		return;

	ImGuiStyle &style = ImGui::GetStyle();
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	ImVec2 p0(x, y), p1(x + w, y + h);

	/* Скругление — заметно сильнее темы (пожелание пользователя):
	   30% высоты кнопки. Заливка полупрозрачная (поверх игры). */
	float rounding = h * 0.3f;

	/* Нажатая — ButtonActive + обводка accent.line (спека: активные
	   состояния — акцентные), обычная — полупрозрачный Button. */
	if (pressed)
	{
		dl->AddRectFilled(p0, p1, ThemeColor(Theme::ButtonActive, 0.40f), rounding);
		dl->AddRect(p0, p1, ThemeColor(Theme::AccentLine, 0.9f), rounding,
			0, style.FrameBorderSize);
	}
	else
	{
		dl->AddRectFilled(p0, p1, ThemeColor(Theme::FrameHover, 0.20f), rounding);
		dl->AddRect(p0, p1, ThemeColor(Theme::Border, 0.75f), rounding,
			0, style.FrameBorderSize);
	}

	if (label != NULL)
	{
		ImVec2 ts = ImGui::CalcTextSize(label);
		dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f),
			ThemeColor(Theme::Text, pressed ? 1.0f : 0.9f), label);
	}
}

void AuroraImgui_StickCircle(float cx, float cy, float r, bool knob)
{
	if (!l_ready)
		return;

	ImGuiStyle &style = ImGui::GetStyle();
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	if (knob)
	{
		dl->AddCircleFilled(ImVec2(cx, cy), r, ThemeColor(Theme::Accent, 0.5f));
		dl->AddCircle(ImVec2(cx, cy), r, ThemeColor(Theme::AccentLine, 0.8f),
			0, style.FrameBorderSize);
	}
	else
	{
		dl->AddCircleFilled(ImVec2(cx, cy), r, ThemeColor(Theme::BgWindow, 0.35f));
		dl->AddCircle(ImVec2(cx, cy), r, ThemeColor(Theme::Border, 0.5f),
			0, style.WindowBorderSize);
	}
}
