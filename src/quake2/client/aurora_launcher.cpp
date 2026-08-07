/*
 * Реализация in-process лаунчера на Dear ImGui. См. aurora_launcher.h.
 *
 * За основу взят эталон gameport/examples/launcher/launcher.cpp, но вместо
 * SDL_GL_CreateContext используется инфраструктура движка: окно —
 * sdlwCreateWindow (SDLWrapper), GL — eglwInitialize/eglwSwapBuffers
 * (EGLWrapper). Тема и шрифт — общие с in-game оверлеем (aurora_imgui).
 *
 * Тач-ввод: SDL на Авроре шлёт события пальца (tfinger, нормированные
 * 0..1), а не мышь (синтез мыши из тача выключен хинтом в Launcher_Run) —
 * тап (<16 px) превращается в mouse down+up, драг — в импульсы MouseWheel
 * для прокрутки списков imgui.
 *
 * Активно только под AURORA_OS; на остальных платформах файл пустой.
 */

#include "aurora_launcher.h"

#if defined(AURORA_OS)

/* Заголовки wrapper'ов — чистый C без extern "C", оборачиваем сами. */
extern "C" {
#include "SDL/SDLWrapper.h"
#include "OpenGLES/EGLWrapper.h"
}
#include "aurora_imgui.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include <GLES2/gl2.h>

#include <SDL_misc.h> /* SDL_OpenURL */

/* Noto Sans Bold (кириллица), сжатый stb-массив — тот же, что у оверлея. */
#include "../imgui/fonts/NotoSansBold.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <pwd.h>

#include <string>
#include <vector>
#include <algorithm>

/* Идентификаторы песочницы (приходят из CMake как строковые дефайны). */
#ifndef AURORA_ORG
#define AURORA_ORG "ru.sashikknox"
#endif
#ifndef AURORA_APP
#define AURORA_APP "quake2"
#endif

namespace {

/* Элементы лаунчера на 20% компактнее шрифтовой шкалы: по фидбеку при
   нормальном размере шрифта сами элементы были слишком крупными. Все
   размеры элементов считаются от ES() вместо ImGui::GetFontSize()
   (шрифт при этом не масштабируется), а метрики стиля темы дополнительно
   умножаются на kElemScale после ApplyTheme (см. Launcher_Run). */
static const float kElemScale = 0.8f;
static inline float ES() { return ImGui::GetFontSize() * kElemScale; }

/* Дополнительное ужатие высот по фидбеку (шрифт не трогаем):
   кнопки — ещё на 20% ниже, вкладки — ещё на 30% ниже. */
static const float kBtnScale = 0.8f;
static const float kTabScale = 0.7f;

/* ---------------------------------------------------------------------------
 * Состояние выбора ресурсов и результата лаунчера.
 * ------------------------------------------------------------------------- */
struct PickerState
{
	std::string current_dir; /* каталог, открытый в браузере */
	std::string selected;    /* подтверждённый корень ресурсов */
	bool        browser_open = false;
	bool        valid_pick   = false; /* selected содержит baseq2/pak0.pak */
	/* Обнаруженные дополнения (сканируются один раз при смене selected,
	   иначе HasMod каждый кадр спамит лог и дёргает ФС). */
	bool        mod_xatrix   = false;
	bool        mod_rogue    = false;
	bool        mod_ctf      = false;
};

PickerState g_picker;

/* Отложенное открытие браузера каталогов: кнопка «Выбрать папку...»
   срабатывает на release тапа, а браузер рисуется в том же кадре позже —
   без отсрочки тот же release «проваливался» в только что открывшееся
   окно и кликал кнопку под пальцем. Флаг выставляется по клику,
   открытие — в начале следующего кадра (DrawLauncherUI). */
bool g_browser_open_pending = false;

/* Результат: куда стартуем ("" = baseq2, иначе имя мода). */
bool        g_launch = false;
std::string g_launch_mod;

/* Тач: тап vs драг. Движение пальца НЕ превращаем в движение мыши (иначе
   кнопки залипают в drag-select): драг — это MouseWheel-импульсы в hovered
   окно imgui, тап — одиночный down+up в точке касания. */
struct TouchState
{
	bool  active     = false;
	float start_x    = 0.f, start_y = 0.f;
	float last_x     = 0.f, last_y  = 0.f;
	float total_dist = 0.f;
};
TouchState g_touch;
float      g_scroll_pending_px = 0.f;

/* ---------------------------------------------------------------------------
 * Файловые утилиты и валидация корня ресурсов.
 * ------------------------------------------------------------------------- */
bool PathExists( const std::string &p )
{
	struct stat st;
	return stat( p.c_str(), &st ) == 0;
}

bool IsDir( const std::string &p )
{
	struct stat st;
	return stat( p.c_str(), &st ) == 0 && S_ISDIR( st.st_mode );
}

/* Корень ресурсов валиден, если в нём есть baseq2/pak0.pak. */
bool ValidateResourceDir( const std::string &dir )
{
	if( !IsDir( dir )) return false;
	return PathExists( dir + "/baseq2/pak0.pak" );
}

/* Дополнение (мод) наличествует, если в <dir>/<mod>/ есть хотя бы один
   .pak (не обязательно pak0.pak — у пользователей бывают разные наборы). */
bool HasMod( const std::string &dir, const char *mod )
{
	std::string sub = dir + "/" + mod;
	DIR *d = opendir( sub.c_str());
	if( !d ) return false;
	bool found = false;
	dirent *ent;
	while(( ent = readdir( d )))
	{
		const char *n = ent->d_name;
		size_t len = strlen( n );
		if( len > 4 && strcasecmp( n + len - 4, ".pak" ) == 0 )
		{
			found = true;
			break;
		}
	}
	closedir( d );
	printf( "Launcher: мод '%s' в '%s': %s\n", mod, sub.c_str(), found ? "найден" : "не найден" );
	return found;
}

/* Сканирование дополнений — один раз при смене выбранной папки. */
void RescanMods()
{
	g_picker.mod_xatrix = false;
	g_picker.mod_rogue  = false;
	g_picker.mod_ctf    = false;
	if( g_picker.selected.empty())
		return;
	g_picker.mod_xatrix = HasMod( g_picker.selected, "xatrix" );
	g_picker.mod_rogue  = HasMod( g_picker.selected, "rogue" );
	g_picker.mod_ctf    = HasMod( g_picker.selected, "ctf" );
}

/* ---------------------------------------------------------------------------
 * Конфиг лаунчера: ~/.config/<org>/<app>/launcher.conf, key=value.
 *   path       — последний выбранный корень ресурсов
 *   r_3d_scale — множитель разрешения рендера (FBO)
 * ------------------------------------------------------------------------- */
std::string HomeDir()
{
	const char *home = getenv( "HOME" );
	if( !home || !*home )
	{
		passwd *pw = getpwuid( getuid());
		home = pw ? pw->pw_dir : "/tmp";
	}
	return std::string( home );
}

std::string ConfigDir()
{
	/* Песочница Авроры разрешает запись только в ~/.config/<org>/<app>
	   (и ~/.local/share, ~/.cache) — иначе конфиг молча теряется. */
	return HomeDir() + "/.config/" AURORA_ORG "/" AURORA_APP;
}

std::string ConfigFile() { return ConfigDir() + "/launcher.conf"; }

void MakeDirsP( const std::string &path )
{
	std::string acc;
	for( size_t i = 1; i <= path.size(); ++i )
	{
		if( i == path.size() || path[i] == '/' )
		{
			acc.assign( path, 0, i );
			if( !acc.empty()) mkdir( acc.c_str(), 0755 );
		}
	}
}

struct LauncherSettings
{
	std::string path;
	float       r_3d_scale = 1.0f;
};

LauncherSettings g_settings;

std::string TrimStr( const std::string &s )
{
	size_t a = 0, b = s.size();
	while( a < b && ( s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n' )) ++a;
	while( b > a && ( s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n' )) --b;
	return s.substr( a, b - a );
}

void LoadSettings()
{
	FILE *f = fopen( ConfigFile().c_str(), "rb" );
	if( !f ) return;
	char line[4096];
	while( fgets( line, sizeof( line ), f ))
	{
		std::string s = TrimStr( line );
		if( s.empty() || s[0] == '#' ) continue;
		size_t eq = s.find( '=' );
		if( eq == std::string::npos ) continue;
		std::string k = TrimStr( s.substr( 0, eq ));
		std::string v = TrimStr( s.substr( eq + 1 ));
		if( k == "path" )            g_settings.path = v;
		else if( k == "r_3d_scale" ) g_settings.r_3d_scale = (float)atof( v.c_str());
	}
	fclose( f );
	if( g_settings.r_3d_scale < 0.25f ) g_settings.r_3d_scale = 0.25f;
	if( g_settings.r_3d_scale > 2.0f )  g_settings.r_3d_scale = 2.0f;
}

void SaveSettings()
{
	MakeDirsP( ConfigDir());
	FILE *f = fopen( ConfigFile().c_str(), "wb" );
	if( !f ) return;
	fprintf( f, "path=%s\n",         g_settings.path.c_str());
	fprintf( f, "r_3d_scale=%.3f\n", g_settings.r_3d_scale );
	fclose( f );
}

/* Стартовый каталог браузера: выбранный путь, иначе стандартный для Авроры
   ~/Downloads/Games/Quake2, иначе ~/Downloads. */
std::string DefaultBrowserDir()
{
	if( !g_picker.selected.empty()) return g_picker.selected;
	std::string home = HomeDir();
	if( IsDir( home + "/Downloads/Games/Quake2" )) return home + "/Downloads/Games/Quake2";
	if( IsDir( home + "/Downloads" ))              return home + "/Downloads";
	return home;
}

/* ---------------------------------------------------------------------------
 * Тач-события: тап vs драг (координаты пальца нормированные 0..1).
 * ------------------------------------------------------------------------- */
void ProcessTouchEvent( const SDL_Event &in, int win_w, int win_h )
{
	if( in.type != SDL_FINGERDOWN && in.type != SDL_FINGERUP && in.type != SDL_FINGERMOTION )
		return;

	/* tfinger.x/y у нас нормированные 0..1 — в пиксели окна. */
	float px = in.tfinger.x * (float)win_w;
	float py = in.tfinger.y * (float)win_h;

	const float TAP_THRESHOLD = 16.f;

	if( in.type == SDL_FINGERDOWN )
	{
		g_touch.active     = true;
		g_touch.start_x    = px;
		g_touch.start_y    = py;
		g_touch.last_x     = px;
		g_touch.last_y     = py;
		g_touch.total_dist = 0.f;

		/* Подводим «мышь» imgui под палец, чтобы hover-тест выбрал
		   правильное окно для последующей прокрутки. */
		SDL_Event mv = {};
		mv.type = SDL_MOUSEMOTION;
		mv.motion.timestamp = in.tfinger.timestamp;
		mv.motion.which     = SDL_TOUCH_MOUSEID;
		mv.motion.x         = (int)px;
		mv.motion.y         = (int)py;
		SDL_PushEvent( &mv );
	}
	else if( in.type == SDL_FINGERMOTION && g_touch.active )
	{
		float dx = px - g_touch.last_x;
		float dy = py - g_touch.last_y;
		g_touch.total_dist += sqrtf( dx * dx + dy * dy );
		g_touch.last_x = px;
		g_touch.last_y = py;
		/* Палец заметно сдвинулся — жест считаем прокруткой. */
		if( g_touch.total_dist >= TAP_THRESHOLD )
			g_scroll_pending_px += dy;
	}
	else if( in.type == SDL_FINGERUP && g_touch.active )
	{
		if( g_touch.total_dist < TAP_THRESHOLD )
		{
			/* Тап — down+up в исходной точке, чтобы imgui увидел клик. */
			SDL_Event d = {};
			d.type = SDL_MOUSEBUTTONDOWN;
			d.button.timestamp = in.tfinger.timestamp;
			d.button.which     = SDL_TOUCH_MOUSEID;
			d.button.button    = SDL_BUTTON_LEFT;
			d.button.state     = SDL_PRESSED;
			d.button.clicks    = 1;
			d.button.x         = (int)g_touch.start_x;
			d.button.y         = (int)g_touch.start_y;
			SDL_PushEvent( &d );

			SDL_Event u = d;
			u.type = SDL_MOUSEBUTTONUP;
			u.button.state = SDL_RELEASED;
			SDL_PushEvent( &u );
		}
		g_touch.active = false;
	}
}

void ApplyPendingScroll()
{
	if( g_scroll_pending_px == 0.f ) return;
	ImGuiIO &io = ImGui::GetIO();
	/* Пиксели -> тики колеса: imgui крутит 5*FontSize px на тик
	   (UpdateMouseWheel: scroll_step = min(5*FontSize, 0.67*высоты окна)).
	   Делим на этот шаг, чтобы контент двигался 1:1 за пальцем. */
	const float px_per_tick = 5.f * ImGui::GetFontSize();
	io.MouseWheel += g_scroll_pending_px / px_per_tick;
	g_scroll_pending_px = 0.f;
}

/* ---------------------------------------------------------------------------
 * Список подкаталогов для браузера.
 * ------------------------------------------------------------------------- */
std::vector<std::string> ListSubdirs( const std::string &dir )
{
	std::vector<std::string> out;
	DIR *d = opendir( dir.c_str());
	if( !d ) return out;
	dirent *ent;
	while(( ent = readdir( d )))
	{
		const char *n = ent->d_name;
		if( n[0] == '.' ) continue; /* скрытые и . / .. пропускаем */
		std::string full = dir + "/" + n;
		if( IsDir( full )) out.push_back( n );
	}
	closedir( d );
	std::sort( out.begin(), out.end(), []( const std::string &a, const std::string &b )
	{
		return strcasecmp( a.c_str(), b.c_str()) < 0;
	});
	return out;
}

std::string ParentOf( const std::string &dir )
{
	if( dir.empty() || dir == "/" ) return "/";
	size_t s = dir.find_last_of( '/' );
	if( s == std::string::npos || s == 0 ) return "/";
	return dir.substr( 0, s );
}

/* ---------------------------------------------------------------------------
 * Браузер каталогов (поверх основного окна лаунчера).
 * ------------------------------------------------------------------------- */
void DrawDirectoryBrowser( int win_w, int win_h )
{
	if( !g_picker.browser_open ) return;

	const float fs = ES();
	ImGui::SetNextWindowPos(  ImVec2( fs * 0.5f, fs * 0.5f ));
	ImGui::SetNextWindowSize( ImVec2( win_w - fs, win_h - fs ));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoSavedSettings;
	ImGui::Begin( "##browser", nullptr, flags );

	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( fs * 0.6f, fs * 0.5f ));
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,  ImVec2( fs * 0.4f, fs * 0.6f ));

	ImGui::Text( "Текущий путь:" );
	ImGui::TextWrapped( "%s", g_picker.current_dir.c_str());
	ImGui::Spacing();

	const float row_h = fs * 2.5f * kBtnScale;

	/* Кнопки действий — во всю ширину, вертикальной стопкой (тач). */
	if( ImGui::Button( "Выбрать эту папку", ImVec2( -1, row_h )))
	{
		g_picker.selected     = g_picker.current_dir;
		g_picker.valid_pick   = ValidateResourceDir( g_picker.selected );
		g_picker.browser_open = false;
		RescanMods();
	}
	if( ImGui::Button( "Вверх", ImVec2( -1, row_h )))
		g_picker.current_dir = ParentOf( g_picker.current_dir );
	if( ImGui::Button( "Отмена", ImVec2( -1, row_h )))
		g_picker.browser_open = false;

	ImGui::Separator();

	/* Подкаталоги — плоским списком (визуально НЕ кнопки): Selectable без
	   фона, текст выровнен влево, перед именем маркер "[  ]" (эмодзи в
	   шрифте нет — диапазоны глифов их не включают). Первым пунктом всегда
	   "[  ] .." — выход на уровень вверх. Каждый пункт отодвинут от краёв
	   scroll view на 3 мм слева и справа (шрифт лаунчера = 4.5 мм, считаем
	   от реального размера шрифта, без ужатия ES). */
	const float pad_x  = ImGui::GetFontSize() * ( 3.f / 4.5f );
	const float item_h = row_h * 0.9f;
	ImGui::BeginChild( "##dir_list", ImVec2( 0, 0 ), false,
		ImGuiWindowFlags_AlwaysVerticalScrollbar );

	/* Подсветка пункта — только пока палец на экране. После FINGERUP
	   синтетическая мышь остаётся в точке тапа, и пункт с тем же индексом
	   в новом списке остаётся "hovered" — без касания гасим hover/active. */
	const bool highlight = g_touch.active;
	if( !highlight )
	{
		ImGui::PushStyleColor( ImGuiCol_HeaderHovered, ImVec4( 0, 0, 0, 0 ));
		ImGui::PushStyleColor( ImGuiCol_HeaderActive,  ImVec4( 0, 0, 0, 0 ));
	}

	auto drawItem = [&]( const char *label ) -> bool
	{
		ImGui::SetCursorPosX( ImGui::GetCursorPosX() + pad_x );
		return ImGui::Selectable( label, false, 0,
			ImVec2( ImGui::GetContentRegionAvail().x - pad_x, item_h ));
	};

	if( drawItem( "[  ] .." ))
		g_picker.current_dir = ParentOf( g_picker.current_dir );

	auto subs = ListSubdirs( g_picker.current_dir );
	for( const auto &name : subs )
	{
		std::string label = "[  ] " + name;
		if( drawItem( label.c_str()))
		{
			std::string next = g_picker.current_dir;
			if( next != "/" ) next += "/";
			next += name;
			g_picker.current_dir = next;
		}
	}

	if( !highlight )
		ImGui::PopStyleColor( 2 );

	ImGui::EndChild();

	ImGui::PopStyleVar( 2 );
	ImGui::End();
}

/* ---------------------------------------------------------------------------
 * Вкладка «Игра».
 * ------------------------------------------------------------------------- */
void StartGame( const char *mod )
{
	g_launch     = true;
	g_launch_mod = mod;
}

void DrawTab_Game()
{
	const float fs    = ES();
	const float btn_h = fs * 3.3f * kBtnScale;

	ImGui::Text( "Путь к ресурсам:" );
	ImGui::TextWrapped( "%s", g_picker.selected.empty()
		? "(не выбран)"
		: g_picker.selected.c_str());

	ImGui::Spacing();
	if( ImGui::Button( "Выбрать папку...", ImVec2( -1, btn_h )))
		g_browser_open_pending = true; /* откроется на следующем кадре */

	ImGui::Spacing();
	if( g_picker.selected.empty())
	{
		ImGui::TextColored( ImVec4( 0.9f, 0.7f, 0.2f, 1.f ),
			"Выберите папку с ресурсами игры" );
	}
	else if( g_picker.valid_pick )
	{
		ImGui::TextColored( ImVec4( 0.4f, 0.9f, 0.4f, 1.f ),
			"Ресурсы найдены." );
	}
	else
	{
		ImGui::TextColored( ImVec4( 0.95f, 0.4f, 0.4f, 1.f ),
			"В выбранной папке не найдены ресурсы игры." );
	}

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));

	const bool can_launch = !g_picker.selected.empty() && g_picker.valid_pick;
	ImGui::BeginDisabled( !can_launch );

	if( ImGui::Button( "Quake II", ImVec2( -1, btn_h )))
		StartGame( "" );

	/* Кнопки дополнений показываем всегда; если pak'и мода не обнаружены
	   (кэш RescanMods) — кнопка неактивна. Порядок — по запросу
	   пользователя: Ground Zero, затем ctf и xatrix. */
	struct ModButton { const char *label; const char *id; bool present; };
	const ModButton mods[] = {
		{ "Ground Zero (rogue)",    "rogue",  g_picker.mod_rogue  },
		{ "Capture The Flag (ctf)", "ctf",    g_picker.mod_ctf    },
		{ "The Reckoning (xatrix)", "xatrix", g_picker.mod_xatrix },
	};
	for( size_t i = 0; i < sizeof( mods ) / sizeof( mods[0] ); ++i )
	{
		ImGui::BeginDisabled( !mods[i].present );
		if( ImGui::Button( mods[i].label, ImVec2( -1, btn_h )))
			StartGame( mods[i].id );
		ImGui::EndDisabled();
	}
	ImGui::TextDisabled( "Дополнения активны при наличии <мод>/*.pak в папке ресурсов" );

	ImGui::EndDisabled();
}

/* ---------------------------------------------------------------------------
 * Вкладка «Настройки».
 * ------------------------------------------------------------------------- */
void DrawTab_Settings()
{
	const float fs = ES();

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));
	ImGui::TextWrapped( "Разрешение рендера (3D scale)" );
	ImGui::TextWrapped(
		"Множитель размера буфера рендеринга относительно экрана. "
		"0.5 = половина разрешения (быстрее), 1.0 = полное, "
		"2.0 = supersampling (чётче, но медленнее)." );
	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));

	ImGui::PushItemWidth( -fs * 4.f );
	ImGui::SliderFloat( "##r_3d_scale", &g_settings.r_3d_scale, 0.25f, 2.0f, "%.2f" );
	ImGui::PopItemWidth();

	ImGui::Spacing();
	/* Пресеты — крупные кнопки под палец. */
	const float btn_h = fs * 3.f * kBtnScale;
	const float btn_w = (( ImGui::GetContentRegionAvail().x - fs * 2.f ) / 5.f );
	auto preset = [&]( const char *label, float value )
	{
		if( ImGui::Button( label, ImVec2( btn_w, btn_h )))
			g_settings.r_3d_scale = value;
	};
	preset( "0.25", 0.25f ); ImGui::SameLine();
	preset( "0.50", 0.50f ); ImGui::SameLine();
	preset( "0.75", 0.75f ); ImGui::SameLine();
	preset( "1.0",  1.00f ); ImGui::SameLine();
	preset( "2.0",  2.00f );

	ImGui::Dummy( ImVec2( 0, fs ));
	ImGui::TextColored( ImVec4( 0.6f, 0.8f, 1.f, 1.f ),
		"Текущее значение: %.2f", g_settings.r_3d_scale );
}

/* ---------------------------------------------------------------------------
 * Вкладка «О программе».
 * ------------------------------------------------------------------------- */
void DrawTab_About()
{
	ImGui::TextWrapped(
		"Программа предоставляется AS IS, без каких-либо гарантий.\n\n"
		"Ресурсы игры не распространяются с этим приложением — "
		"приобретите игру легально и укажите путь к ним во вкладке «Игра»." );
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Движок Quake II (id Software), лицензия GPL.\n"
		"Порт для ОС Аврора — sashikknox." );
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Подписывайтесь в телеграм. Свежие порты игр и новости об ОС Аврора:" );

	/* Кликабельная ссылка на телеграм-канал: подчёркнутый текст цветом
	   accent (#3B82F6), по тапу открываем через SDL_OpenURL. */
	{
		const ImVec4 accent( 0x3B / 255.f, 0x82 / 255.f, 0xF6 / 255.f, 1.f );
		ImGui::PushStyleColor( ImGuiCol_Text, accent );
		ImGui::TextUnformatted( "sashikknox Все портит! — t.me/auroraosgames" );
		ImGui::PopStyleColor();
		ImVec2 rmin = ImGui::GetItemRectMin();
		ImVec2 rmax = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2( rmin.x, rmax.y ), ImVec2( rmax.x, rmax.y ),
			ImGui::GetColorU32( accent ));
		if( ImGui::IsItemClicked())
			SDL_OpenURL( "https://t.me/auroraosgames" );
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Сторонние компоненты:\n"
		"  Dear ImGui (c) Omar Cornut, лицензия MIT\n"
		"  SDL2, лицензия zlib\n"
		"  zlib, лицензия zlib" );
}

/* ---------------------------------------------------------------------------
 * Строка вкладок: кастомные кнопки в стиле темы (стандартный TabBar imgui
 * синий и сюда не вписывается), скругление только сверху. Сама строка
 * фиксирована — прокручивается только контент под ней.
 * ------------------------------------------------------------------------- */
int g_active_tab = 0;

void DrawTabsRow()
{
	static const char *names[] = { "Игра", "Настройки", "О программе" };
	const float fs       = ES();
	const float tab_h    = fs * 2.6f * kTabScale;
	const float gap      = fs * 0.4f;
	const float avail    = ImGui::GetContentRegionAvail().x;
	const float tab_w    = ( avail - gap * 2.f ) / 3.f;
	const float rounding = fs * 0.8f;

	/* Цвета — из активной темы: активная вкладка акцентная, остальные —
	   как неактивные фреймы. */
	const ImVec4 accent  = ImGui::GetStyleColorVec4( ImGuiCol_SliderGrab );
	const ImVec4 normal  = ImGui::GetStyleColorVec4( ImGuiCol_FrameBg );
	const ImVec4 hovered = ImGui::GetStyleColorVec4( ImGuiCol_FrameBgHovered );

	ImDrawList *dl = ImGui::GetWindowDrawList();
	for( int i = 0; i < 3; i++ )
	{
		if( i > 0 )
			ImGui::SameLine( 0.f, gap );

		ImGui::PushID( i );
		ImGui::InvisibleButton( "##tab", ImVec2( tab_w, tab_h ));
		const bool sel = ( g_active_tab == i );
		if( ImGui::IsItemClicked())
			g_active_tab = i;

		const ImVec4 col  = sel ? accent : ( ImGui::IsItemHovered() ? hovered : normal );
		const ImVec2 rmin = ImGui::GetItemRectMin();
		const ImVec2 rmax = ImGui::GetItemRectMax();
		dl->AddRectFilled( rmin, rmax, ImGui::GetColorU32( col ),
			rounding, ImDrawFlags_RoundCornersTop );

		const ImVec2 ts = ImGui::CalcTextSize( names[i] );
		dl->AddText( ImVec2( rmin.x + ( tab_w - ts.x ) * 0.5f,
				rmin.y + ( tab_h - ts.y ) * 0.5f ),
			ImGui::GetColorU32( ImGuiCol_Text ), names[i] );
		ImGui::PopID();
	}
}

/* ---------------------------------------------------------------------------
 * Корневое окно лаунчера (полноэкранное, без декораций).
 * ------------------------------------------------------------------------- */
void DrawLauncherUI( bool &user_quit, int win_w, int win_h )
{
	/* Отложенное открытие браузера — здесь, в начале кадра: release тапа,
	   открывшего его, остался в прошлом кадре и в окно не провалится. */
	if( g_browser_open_pending )
	{
		g_browser_open_pending = false;
		g_picker.current_dir   = DefaultBrowserDir();
		g_picker.browser_open  = true;
	}

	ImGui::SetNextWindowPos(  ImVec2( 0, 0 ));
	ImGui::SetNextWindowSize( ImVec2( (float)win_w, (float)win_h ));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoSavedSettings;

	ImGui::Begin( "##launcher", nullptr, flags );

	const float fs = ES();
	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( fs * 1.0f, fs * 0.6f ));
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,  ImVec2( fs * 0.5f, fs * 0.6f ));

	ImVec2 hdr = ImGui::CalcTextSize( "Quake 2 (AuroraOS)" );
	ImGui::SetCursorPos( ImVec2(( win_w - hdr.x ) * 0.5f, fs * 1.2f ));
	ImGui::TextUnformatted( "Quake 2 (AuroraOS)" );

	/* Строка вкладок — фиксированная, прокрутке не подлежит. */
	DrawTabsRow();
	ImGui::Spacing();

	/* Контент активной вкладки — в прокручиваемой области: с крупным
	   шрифтом и кнопками DLC всё может не поместиться по высоте. */
	const float exit_h = fs * 3.3f * kBtnScale;
	const float avail_h = win_h - ImGui::GetCursorPosY() - exit_h
		- fs * 1.5f - ImGui::GetStyle().ItemSpacing.y;
	ImGui::BeginChild( "##content", ImVec2( 0, avail_h ), false );

	switch( g_active_tab )
	{
	case 0:  DrawTab_Game();     break;
	case 1:  DrawTab_Settings(); break;
	default: DrawTab_About();    break;
	}

	ImGui::EndChild();

	/* «Выход» — внизу, во всю ширину. */
	ImGui::SetCursorPosY( win_h - exit_h - fs * 1.5f );
	if( ImGui::Button( "Выход", ImVec2( -1, exit_h )))
		user_quit = true;

	ImGui::PopStyleVar( 2 );
	ImGui::End();

	DrawDirectoryBrowser( win_w, win_h );
}

/* Один кадр «ЗАГРУЗКА» перед передачей управления движку: инициализация
   движка и загрузка pak'ов занимают заметное время, а застывший кадр с
   нажатой кнопкой выглядит сломанным. */
void DrawLoadingFrame( SDL_Window *window )
{
	int win_w = 0, win_h = 0;
	SDL_GetWindowSize( window, &win_w, &win_h );

	/* Сливаем очередь событий, чтобы NewFrame увидел чистое состояние. */
	SDL_Event drain;
	while( SDL_PollEvent( &drain ))
		ImGui_ImplSDL2_ProcessEvent( &drain );

	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();

	ImGui::SetNextWindowPos(  ImVec2( 0, 0 ));
	ImGui::SetNextWindowSize( ImVec2( (float)win_w, (float)win_h ));
	ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoScrollbar;
	ImGui::Begin( "##loading", nullptr, wflags );
	const char *txt = "ЗАГРУЗКА";
	ImVec2 sz = ImGui::CalcTextSize( txt );
	ImGui::SetCursorPos( ImVec2(( win_w - sz.x ) * 0.5f, ( win_h - sz.y ) * 0.5f ));
	ImGui::TextUnformatted( txt );
	ImGui::End();

	ImGui::Render();
	glViewport( 0, 0, win_w, win_h );
	glClearColor( 0.08f, 0.08f, 0.10f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
	eglwSwapBuffers();
}

} // namespace

extern "C" int Launcher_Run( void )
{
	/* Не даём SDL синтезировать мышь из тача: лаунчер сам конвертирует
	   тапы/драги (ProcessTouchEvent), а при включённом синтезе каждый тап
	   приходил дважды (пара от SDL + наша) — двойные клики по кнопкам.
	   Ставим ДО SDL_Init(SDL_INIT_VIDEO): значение хинта подхватывается
	   колбеком при инициализации мыши. Движок позже выставляет тот же
	   хинт в IN_Init (input_sdl.c) — конфликта нет. */
	SDL_SetHint( SDL_HINT_TOUCH_MOUSE_EVENTS, "0" );

	/* sdlwInitialize вызван в Qcommon_Init с нулевыми флагами — подсистему
	   видео при необходимости поднимаем здесь. */
	if( SDL_WasInit( SDL_INIT_VIDEO ) == 0 )
	{
		if( SDL_Init( SDL_INIT_VIDEO ) < 0 )
		{
			fprintf( stderr, "Launcher: SDL_Init failed: %s\n", SDL_GetError());
			return 1;
		}
	}

	/* Окно — тем же способом и с теми же флагами, что движок
	   (r_main.c, R_Window_update): SDL_WINDOW_OPENGL нужен, чтобы SDL под
	   Wayland создал wl_egl_window для EGLWrapper'а. */
	/* Размер окна — той же логикой, что у движка (R_Window_getMaxWindowSize):
	   СНАЧАЛА usable bounds (на портретной панели это 1080x2400 — уже с
	   учётом transform композитора), иначе desktop mode. НЕ использовать
	   голый SDL_GetDesktopDisplayMode: на этом устройстве он отдаёт
	   нативный ландшафт панели (2400x1080) без поворота — окно получается
	   ландшафтным и блит FBO рисуется в неверный viewport. */
	int win_w = 1080, win_h = 1920;
	{
		SDL_Rect bounds;
		if( SDL_GetDisplayUsableBounds( 0, &bounds ) == 0 && bounds.w > 0 && bounds.h > 0 )
		{
			win_w = bounds.w;
			win_h = bounds.h;
		}
		else
		{
			SDL_DisplayMode dm;
			if( SDL_GetDesktopDisplayMode( 0, &dm ) == 0 )
			{
				win_w = dm.w;
				win_h = dm.h;
			}
		}
		printf( "Launcher: window %dx%d\n", win_w, win_h );
		fflush( stdout );
	}
	if( sdlwCreateWindow( "Quake 2", win_w, win_h,
		SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN ))
	{
		fprintf( stderr, "Launcher: sdlwCreateWindow failed\n" );
		return 1;
	}
	SDL_Window *window = sdlwContext->window;

	/* Диагностика геометрии сразу после создания окна. */
	{
		int ww = 0, wh = 0, dw = 0, dh = 0;
		SDL_DisplayMode cur;
		SDL_GetWindowSize( window, &ww, &wh );
		SDL_GL_GetDrawableSize( window, &dw, &dh );
		printf( "Launcher: запущен, окно %dx%d, drawable %dx%d\n", ww, wh, dw, dh );
		if( SDL_GetCurrentDisplayMode( 0, &cur ) == 0 )
			printf( "Launcher: current display mode %dx%d\n", cur.w, cur.h );
		fflush( stdout );
	}

	/* EGL — конфиги как в R_Window_createContext (r_main.c), но без MSAA.
	   Контекст остаётся жить после лаунчера: движок его подхватит. */
	{
		EglwConfigInfo minimal;
		minimal.redSize = 5; minimal.greenSize = 5; minimal.blueSize = 5; minimal.alphaSize = 0;
		minimal.depthSize = 16; minimal.stencilSize = 0; minimal.samples = 0;
		EglwConfigInfo requested;
		requested.redSize = 5; requested.greenSize = 5; requested.blueSize = 5; requested.alphaSize = 0;
		requested.depthSize = 16; requested.stencilSize = 1; requested.samples = 0;
		if( eglwInitialize( &minimal, &requested, false ))
		{
			fprintf( stderr, "Launcher: eglwInitialize failed\n" );
			return 1;
		}
	}
	eglSwapInterval( eglwContext->display, 1 );

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr; /* imgui.ini не пишем (песочница) */
	io.LogFilename = nullptr;

	/* Размер шрифта от физического DPI: цель ~4.5 мм (кламп 14..96 px,
	   чтобы битый DPI не дал микроскопический/гигантский шрифт).
	   Крупнее, чем in-game оверлей (3 мм) — по фидбеку пользователя
	   текст лаунчера был слишком мелким. */
	float vdpi = 96.f;
	SDL_GetDisplayDPI( SDL_GetWindowDisplayIndex( window ), nullptr, nullptr, &vdpi );
	if( vdpi <= 1.f ) vdpi = 96.f;
	float font_px = 4.5f * ( vdpi / 25.4f );
	if( font_px < 14.f ) font_px = 14.f;
	if( font_px > 96.f ) font_px = 96.f;

	/* Тема — общая с in-game оверлеем (спека imgui-theme-spec.md). */
	AuroraImgui_ApplyTheme( font_px );
	/* Элементы лаунчера на 20% компактнее при том же шрифте (отступы,
	   скругления и прочие метрики темы; явные размеры — через ES()). */
	ImGui::GetStyle().ScaleAllSizes( kElemScale );

	/* Шрифт — Noto Sans Bold (встроенный массив). Диапазоны глифов —
	   явно: Latin-1 + Cyrillic + General Punctuation (тире 0x2014 и пр. —
	   в GetGlyphRangesCyrillic их НЕТ, тире рисовалось «?»). Эмодзи-
	   диапазоны осознанно не берём. */
	static const ImWchar kGlyphRanges[] = {
		0x0020, 0x00FF, /* Latin-1 + пунктуация */
		0x0400, 0x04FF, /* Cyrillic */
		0x2010, 0x205E, /* General Punctuation (— „ “ ” …) */
		0,
	};
	if( io.Fonts->AddFontFromMemoryCompressedTTF(
		NotoSansBold_compressed_data, NotoSansBold_compressed_size,
		font_px, nullptr, kGlyphRanges ) == nullptr )
	{
		/* На всякий случай — встроенный шрифт imgui с масштабом. */
		io.FontGlobalScale = font_px / 13.f;
	}

	/* SDL GL context у нас нет (EGL живёт в EGLWrapper'е) — бэкенду он
	   не нужен (параметр зарезервирован под multi-viewport). */
	ImGui_ImplSDL2_InitForOpenGL( window, nullptr );
	ImGui_ImplOpenGL3_Init();

	/* Конфиг + предвыбор ресурсов: сохранённый путь, иначе стандартный
	   ~/Downloads/Games/Quake2 — если валиден, пользователю достаточно
	   нажать «Начать игру». */
	LoadSettings();
	{
		std::string defq = HomeDir() + "/Downloads/Games/Quake2";
		if( !g_settings.path.empty() && ValidateResourceDir( g_settings.path ))
		{
			g_picker.selected   = g_settings.path;
			g_picker.valid_pick = true;
		}
		else if( ValidateResourceDir( defq ))
		{
			g_picker.selected   = defq;
			g_picker.valid_pick = true;
		}
		RescanMods();
	}

	bool user_quit = false;

	/* Отладочный автостарт (env AURORA_LAUNCHER_AUTO=1): через ~2.5 с
	   программно вызываем тот же колбек, что у кнопки «Quake II». */
	const bool auto_start = getenv( "AURORA_LAUNCHER_AUTO" ) != nullptr;
	const Uint32 loop_start = SDL_GetTicks();
	bool auto_fired = false;

	while( !g_launch && !user_quit )
	{
		SDL_GetWindowSize( window, &win_w, &win_h );

		SDL_Event ev;
		while( SDL_PollEvent( &ev ))
		{
			ImGui_ImplSDL2_ProcessEvent( &ev );

			if( ev.type == SDL_QUIT )
				user_quit = true;
			else if( ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERUP || ev.type == SDL_FINGERMOTION )
				ProcessTouchEvent( ev, win_w, win_h );
		}

		if( auto_start && !auto_fired && SDL_GetTicks() - loop_start >= 2500 )
		{
			auto_fired = true;
			int ww = 0, wh = 0, dw = 0, dh = 0;
			SDL_GetWindowSize( window, &ww, &wh );
			SDL_GL_GetDrawableSize( window, &dw, &dh );
			printf( "Launcher: автостарт (колбек кнопки «Quake II»), окно %dx%d, drawable %dx%d\n",
				ww, wh, dw, dh );
			fflush( stdout );
			StartGame( "" );
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ApplyPendingScroll();
		ImGui::NewFrame();

		DrawLauncherUI( user_quit, win_w, win_h );

		ImGui::Render();
		glViewport( 0, 0, win_w, win_h );
		glClearColor( 0.08f, 0.08f, 0.10f, 1.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );

		eglwSwapBuffers();
	}

	int result = 1;
	if( g_launch )
	{
		/* Передаём выбор движку через env (подхватываются в Qcommon_Init,
		   FS_AddGameDirectories и RFBO_Init). */
		setenv( "AURORA_RESDIR",   g_picker.selected.c_str(), 1 );
		setenv( "AURORA_GAME_MOD", g_launch_mod.c_str(),      1 );
		char scale_buf[32];
		snprintf( scale_buf, sizeof( scale_buf ), "%.3f", g_settings.r_3d_scale );
		setenv( "AURORA_R_3D_SCALE", scale_buf, 1 );

		g_settings.path = g_picker.selected;
		result = 0;
	}
	/* Конфиг сохраняем и при выходе — настройки не должны теряться. */
	SaveSettings();

	if( g_launch )
		DrawLoadingFrame( window );

	/* Выгружаем только imgui (освобождает свои VAO/VBO/программу/текстуру
	   шрифта). Окно и EGL НЕ трогаем — их подхватывает движок. SDL_Quit
	   не вызываем. */
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();

	return result;
}

#endif /* AURORA_OS */
