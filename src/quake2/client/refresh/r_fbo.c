/*
 * FBO-модуль порта на ОС Аврора (этап 2 по gameport/AGENTS.MD). См. r_fbo.h.
 *
 * Поток кадра: RFBO_BindForFrame() в начале R_Frame_begin → движок рисует
 * сцену и UI в FBO (для него FBO выглядит как экран: viddef подменён
 * размером FBO) → RFBO_DrawToScreen() в R_Frame_end выводит текстуру FBO
 * полноэкранным квадом → eglwSwapBuffers().
 *
 * Требования из AGENTS.MD, которые здесь учтены:
 * - без MSAA; вывод только квадом с текстурой (никакого glBlitFramebuffer);
 * - матрицы поворота (enum wl_output_transform) готовятся один раз на старте,
 *   set_rotation лишь переключает активную; в отрисовке кадра нет вычислений
 *   и выделений памяти;
 * - поворот применяется ТОЛЬКО в вершинном шейдере и ТОЛЬКО к позициям;
 *   UV проходят без модификаций;
 * - GL-состояние wrapper'а после блита восстанавливается (program — через
 *   oglwGetProgram, текстура/buffer — к 0, blend — в состояние кэша 2D ROP);
 * - свои attribute locations (14/15), чтобы не пересекаться с wrapper'ом.
 */

#include "r_private.h"
#include "r_fbo.h"

#if defined(AURORA_FBO)

#if defined(AURORA_VR)
#include "client/vr_lens.h"
#endif


/* Attribute locations, гарантированно не пересекающиеся с wrapper'ом
   (a_position/a_color/a_texcoord0/a_texcoord1 — низкие индексы). */
#define RFBO_ATTR_POS 14
#define RFBO_ATTR_UV  15

static struct
{
	GLuint fbo;
	GLuint colorTex;
	GLuint depthStencilRb;
	GLuint program;
	GLint uRot;
	GLint uTex;
	GLint uGamma;
	int screenW, screenH; /* реальный размер окна */
	int fboW, fboH;       /* размер буфера рендеринга */
	float scale;
	int rotation;         /* enum wl_output_transform */
	bool ready;
#if defined(AURORA_VR)
	/* Раскладка глаз под линзы. Параметры задаёт клиент (vr_lens.c),
	   вершины готовятся в RFBO_UpdateLensLayout по событиям; кадр берёт
	   готовые drawVerts/drawCount. */
	bool lensSplit;
	float lensSepMm, lensVofsMm, lensTiltMm;
	float lensWidthMm, lensHeightMm;
	bool lensFromDpi;
	float lensVerts[48];
	const float *drawVerts;
	int drawCount;
#endif
} l_fbo;

/* Полноэкранный квад в NDC: (-1,-1)..(1,1), pos2 + uv2.
   UV не поворачиваются никогда — поворот только геометрии. */
static const float l_quadVerts[] =
{
	-1.0f, -1.0f, 0.0f, 0.0f,
	 1.0f, -1.0f, 1.0f, 0.0f,
	-1.0f,  1.0f, 0.0f, 1.0f,
	-1.0f,  1.0f, 0.0f, 1.0f,
	 1.0f, -1.0f, 1.0f, 0.0f,
	 1.0f,  1.0f, 1.0f, 1.0f,
};

/* Матрицы поворота позиций квада, column-major mat2, индекс = enum
   wl_output_transform (0 NORMAL, 1 = 90° CCW, 2 = 180°, 3 = 270° CCW).
   Готовятся один раз, выбор — в RFBO_SetRotation. */
static const float l_rotMatrices[4][4] =
{
	{ 1.0f, 0.0f,  0.0f, 1.0f }, /* NORMAL */
	{ 0.0f, 1.0f, -1.0f, 0.0f }, /* 90 CCW: (x,y) -> (-y,x) */
	{-1.0f, 0.0f,  0.0f,-1.0f }, /* 180 */
	{ 0.0f,-1.0f,  1.0f, 0.0f }, /* 270 CCW: (x,y) -> (y,-x) */
};

static const char l_vertexShaderSrc[] =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"uniform mat2 u_rot;\n"
	"varying vec2 v_uv;\n"
	"void main()\n"
	"{\n"
	"	gl_Position = vec4(u_rot * a_pos, 0.0, 1.0);\n"
	"	v_uv = a_uv;\n"
	"}\n";

static const char l_fragmentShaderSrc[] =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_gamma;\n"
	"void main()\n"
	"{\n"
	"	vec4 c = texture2D(u_tex, v_uv);\n"
	/* Яркость (меню «brightness» = r_gamma): hardware gamma ramp на
	   Wayland не работает, поэтому гамма применяется здесь, при блите. */
	"	gl_FragColor = vec4(pow(c.rgb, vec3(u_gamma)), c.a);\n"
	"}\n";

static GLuint RFBO_CompileShader(GLenum type, const char *src)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);
	GLint compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled)
	{
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		R_printf(PRINT_ALL, "RFBO: shader compile error: %s\n", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint RFBO_CreateProgram(void)
{
	GLuint vs = RFBO_CompileShader(GL_VERTEX_SHADER, l_vertexShaderSrc);
	GLuint fs = RFBO_CompileShader(GL_FRAGMENT_SHADER, l_fragmentShaderSrc);
	if (vs == 0 || fs == 0)
		return 0;

	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	/* До линковки — свои attribute locations вне диапазона wrapper'а. */
	glBindAttribLocation(program, RFBO_ATTR_POS, "a_pos");
	glBindAttribLocation(program, RFBO_ATTR_UV, "a_uv");
	glLinkProgram(program);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		char log[1024];
		glGetProgramInfoLog(program, sizeof(log), NULL, log);
		R_printf(PRINT_ALL, "RFBO: program link error: %s\n", log);
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

static bool RFBO_CreateTargets(int w, int h)
{
	glGenTextures(1, &l_fbo.colorTex);
	glBindTexture(GL_TEXTURE_2D, l_fbo.colorTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	/* Анбинд — forced через wrapper, чтобы его кэш биндингов не расходился
	   с реальным состоянием (см. комментарий в RFBO_DrawToScreen). */
	oglwBindTextureForced(0, 0);

	glGenRenderbuffers(1, &l_fbo.depthStencilRb);
	glBindRenderbuffer(GL_RENDERBUFFER, l_fbo.depthStencilRb);
	const char *extensions = (const char *)glGetString(GL_EXTENSIONS);
	bool packedDepthStencil = extensions != NULL && strstr(extensions, "GL_OES_packed_depth_stencil") != NULL;
	if (packedDepthStencil)
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, w, h);
	else
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	glGenFramebuffers(1, &l_fbo.fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, l_fbo.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, l_fbo.colorTex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, l_fbo.depthStencilRb);
	if (packedDepthStencil)
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, l_fbo.depthStencilRb);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (status != GL_FRAMEBUFFER_COMPLETE)
	{
		R_printf(PRINT_ALL, "RFBO: framebuffer incomplete (0x%x)\n", status);
		return false;
	}
	return true;
}

static void RFBO_DestroyTargets(void)
{
	if (l_fbo.fbo != 0)
		glDeleteFramebuffers(1, &l_fbo.fbo);
	if (l_fbo.colorTex != 0)
		glDeleteTextures(1, &l_fbo.colorTex);
	if (l_fbo.depthStencilRb != 0)
		glDeleteRenderbuffers(1, &l_fbo.depthStencilRb);
	l_fbo.fbo = 0;
	l_fbo.colorTex = 0;
	l_fbo.depthStencilRb = 0;
}

/* Размер FBO из размера окна: при повороте на 90/270 размеры перевёрнуты
   (ландшафтный контент на портретной панели), масштаб — коэффициентом.
   Размер буфера НЕ зависит от смены ориентации 90↔270 (оба — перевёрнутые),
   поэтому при повороте устройства FBO не пересоздаётся. */
static void RFBO_ComputeSize(int windowWidth, int windowHeight, int *fboW, int *fboH)
{
	bool swapped = (l_fbo.rotation == 1 || l_fbo.rotation == 3);
	int w = swapped ? windowHeight : windowWidth;
	int h = swapped ? windowWidth : windowHeight;
	*fboW = (int)(w * l_fbo.scale + 0.5f);
	*fboH = (int)(h * l_fbo.scale + 0.5f);
	/* При масштабировании округляем до чётных (нечётные размеры не все
	   GPU любят). При scale=1.0 размеры бит-в-бит как раньше. */
	if (l_fbo.scale != 1.0f)
	{
		*fboW &= ~1;
		*fboH &= ~1;
	}
}

#if defined(AURORA_VR)
/*
 * Пересчёт вершин двух квадов глаз. Только по событиям: смена параметров
 * калибровки, поворота, размера окна, дисплея. В RFBO_DrawToScreen — лишь
 * готовый массив.
 *
 * Ширина контента в пикселях экрана зависит от поворота (контент
 * ландшафтный, панель любая — см. RFBO_ComputeSize), поэтому берётся по
 * чётности rotation, а не по допущению «панель портретная».
 */
static void RFBO_UpdateLensLayout(void)
{
	l_fbo.drawVerts = l_quadVerts;
	l_fbo.drawCount = 6;
	if (!l_fbo.lensSplit || l_fbo.screenW <= 0 || l_fbo.screenH <= 0)
		return;

	bool swapped = (l_fbo.rotation & 1) != 0;
	int contentW = swapped ? l_fbo.screenH : l_fbo.screenW;
	int contentH = swapped ? l_fbo.screenW : l_fbo.screenH;

	/* Дисплей — по окну, как в Touch_RefreshDpi. ddpi, а не hdpi/vdpi:
	   в Wayland-бэкенде SDL только диагональ не зависит от transform. */
	int displayIndex = 0;
	if (sdlwContext != NULL && sdlwContext->window != NULL)
	{
		displayIndex = SDL_GetWindowDisplayIndex(sdlwContext->window);
		if (displayIndex < 0)
			displayIndex = 0;
	}
	float ddpi = 0.0f;
	if (SDL_GetDisplayDPI(displayIndex, &ddpi, NULL, NULL) != 0)
		ddpi = 0.0f;

	bool fromDpi = VRL_ContentSizeMm(ddpi, contentW, contentH, &l_fbo.lensWidthMm, &l_fbo.lensHeightMm) != 0;
	if (fromDpi != l_fbo.lensFromDpi || !fromDpi)
	{
		/* Лог только при смене источника (или всегда при фолбэке — это
		   редкое событие и важная диагностика «почему 63 мм не 63 мм»),
		   а не на каждый шаг калибровки. */
		static bool l_loggedFallback = false;
		if (fromDpi || !l_loggedFallback)
			R_printf(PRINT_ALL, "RFBO: lens layout, screen %.0fx%.0f mm (%s, ddpi %.0f)\n",
				l_fbo.lensWidthMm, l_fbo.lensHeightMm, fromDpi ? "dpi" : "fallback", ddpi);
		l_loggedFallback = !fromDpi;
	}
	l_fbo.lensFromDpi = fromDpi;

	float c[4];
	VRL_EyeCenters(l_fbo.lensWidthMm, l_fbo.lensHeightMm,
		l_fbo.lensSepMm, l_fbo.lensVofsMm, l_fbo.lensTiltMm, c);
	l_fbo.drawCount = VRL_BuildEyeQuads(c, l_fbo.lensVerts);
	l_fbo.drawVerts = l_fbo.lensVerts;
}
#endif

bool RFBO_Init(int windowWidth, int windowHeight)
{
	/* rotation выставляется вызывающим кодом ДО Init (от него зависят
	   размеры FBO) — сохраняем его. */
	int rotation = l_fbo.rotation;
#if defined(AURORA_VR)
	/* Параметры линз задаёт клиент один раз при изменении; пересоздание
	   рендера (vid_restart) не должно их терять. */
	bool lensSplit = l_fbo.lensSplit;
	float lensSep = l_fbo.lensSepMm, lensVofs = l_fbo.lensVofsMm, lensTilt = l_fbo.lensTiltMm;
#endif
	memset(&l_fbo, 0, sizeof(l_fbo));
#if defined(AURORA_VR)
	l_fbo.lensSplit = lensSplit;
	l_fbo.lensSepMm = lensSep;
	l_fbo.lensVofsMm = lensVofs;
	l_fbo.lensTiltMm = lensTilt;
	l_fbo.drawVerts = l_quadVerts;
	l_fbo.drawCount = 6;
#endif
	l_fbo.scale = 1.0f;
#if defined(AURORA_OS)
	/* Множитель разрешения рендера из лаунчера (env AURORA_R_3D_SCALE),
	   читаем один раз. RFBO_ComputeSize применит его к размеру FBO
	   (после поворотной логики). */
	{
		const char *s = getenv("AURORA_R_3D_SCALE");
		if (s != NULL && s[0] != '\0')
		{
			float v = (float)atof(s);
			if (v < 0.25f)
				v = 0.25f;
			if (v > 2.0f)
				v = 2.0f;
			l_fbo.scale = v;
		}
	}
#endif
	l_fbo.rotation = rotation;

	l_fbo.program = RFBO_CreateProgram();
	if (l_fbo.program == 0)
		return false;
	l_fbo.uRot = glGetUniformLocation(l_fbo.program, "u_rot");
	l_fbo.uTex = glGetUniformLocation(l_fbo.program, "u_tex");
	l_fbo.uGamma = glGetUniformLocation(l_fbo.program, "u_gamma");

	l_fbo.screenW = windowWidth;
	l_fbo.screenH = windowHeight;
	RFBO_ComputeSize(windowWidth, windowHeight, &l_fbo.fboW, &l_fbo.fboH);

	if (!RFBO_CreateTargets(l_fbo.fboW, l_fbo.fboH))
	{
		RFBO_Shutdown();
		return false;
	}

	/* Подмена размера экрана размером FBO в логике движка (EGL не трогаем). */
	viddef.width = l_fbo.fboW;
	viddef.height = l_fbo.fboH;

	l_fbo.ready = true;
#if defined(AURORA_VR)
	RFBO_UpdateLensLayout();
#endif
	R_printf(PRINT_ALL, "RFBO: %ix%i -> screen %ix%i\n", l_fbo.fboW, l_fbo.fboH, l_fbo.screenW, l_fbo.screenH);
	/* Временная диагностика: что SDL сообщает о дисплее на старте рендера.
	   Дисплей — по окну (перенос на внешний экран), не захардкоженный 0. */
	{
		int displayIndex = 0;
		if (sdlwContext != NULL && sdlwContext->window != NULL)
		{
			displayIndex = SDL_GetWindowDisplayIndex(sdlwContext->window);
			if (displayIndex < 0)
				displayIndex = 0;
		}
		SDL_DisplayMode dm;
		SDL_Rect ub;
		if (SDL_GetDesktopDisplayMode(displayIndex, &dm) == 0)
			R_printf(PRINT_ALL, "RFBO: desktop display mode %ix%i\n", dm.w, dm.h);
		if (SDL_GetDisplayUsableBounds(displayIndex, &ub) == 0)
			R_printf(PRINT_ALL, "RFBO: usable bounds %ix%i\n", ub.w, ub.h);
	}
	return true;
}

void RFBO_Shutdown(void)
{
	RFBO_DestroyTargets();
	if (l_fbo.program != 0)
		glDeleteProgram(l_fbo.program);
	l_fbo.program = 0;
	l_fbo.ready = false;
}

void RFBO_Resize(int windowWidth, int windowHeight)
{
	if (!l_fbo.ready)
		return;

	/* Сравниваем не только размер окна, но и вычисленный размер FBO: он
	   зависит и от rotation (перевёрнутые пропорции на 90/270), поэтому
	   смена поворота при том же размере окна тоже ведёт к пересозданию. */
	int newFboW, newFboH;
	RFBO_ComputeSize(windowWidth, windowHeight, &newFboW, &newFboH);
	if (windowWidth == l_fbo.screenW && windowHeight == l_fbo.screenH &&
		newFboW == l_fbo.fboW && newFboH == l_fbo.fboH)
		return;

	l_fbo.screenW = windowWidth;
	l_fbo.screenH = windowHeight;
	l_fbo.fboW = newFboW;
	l_fbo.fboH = newFboH;

	RFBO_DestroyTargets();
	if (!RFBO_CreateTargets(l_fbo.fboW, l_fbo.fboH))
	{
		RFBO_Shutdown();
		return;
	}

	viddef.width = l_fbo.fboW;
	viddef.height = l_fbo.fboH;
#if defined(AURORA_VR)
	RFBO_UpdateLensLayout();
#endif
	/* Редкое событие (реальный ресайз/перенос на другой дисплей) — можно в лог. */
	R_printf(PRINT_ALL, "RFBO: resize %ix%i -> screen %ix%i rotation %i\n",
		l_fbo.fboW, l_fbo.fboH, l_fbo.screenW, l_fbo.screenH, l_fbo.rotation);
}

void RFBO_BindForFrame(void)
{
	if (!l_fbo.ready)
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, l_fbo.fbo);
	/* Синхронизация кэша viewport'а wrapper'а: после блита реальный
	   viewport — экранный; фиксируем это в кэше, затем сырым вызовом
	   переключаемся на размер FBO. Дальше движок сам выставит viewport
	   через oglwSetViewport от viddef (размер FBO). */
	oglwSetViewport(0, 0, l_fbo.screenW, l_fbo.screenH);
	glViewport(0, 0, l_fbo.fboW, l_fbo.fboH);
}

void RFBO_DrawToScreen(void)
{
	if (!l_fbo.ready)
		return;

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, l_fbo.screenW, l_fbo.screenH);

	/* Состояние для блита (сырьём, wrapper про него не знает —
	   после блита возвращаем всё в состояние кэша wrapper'а). */
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
#if defined(AURORA_VR)
	/* Квады глаз покрывают экран не целиком (сдвиг под линзы) — остальное
	   должно быть чёрным, а не прошлым кадром. Флаг готов заранее
	   (RFBO_UpdateLensLayout); цвет очистки движок выставляет сам перед
	   каждой своей очисткой (R_Frame_clear), кэша у wrapper'а для него нет. */
	if (l_fbo.drawVerts != l_quadVerts)
	{
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
#endif

	glUseProgram(l_fbo.program);
	glUniformMatrix2fv(l_fbo.uRot, 1, GL_FALSE, l_rotMatrices[l_fbo.rotation & 3]);
	glUniform1i(l_fbo.uTex, 0);
	/* Гамма (яркость) — как в R_Gamma_calculateRamp: pow(v, 1/r_gamma). */
	float gamma = r_gamma->value;
	if (gamma <= 0.0f)
		gamma = 1.0f;
	glUniform1f(l_fbo.uGamma, 1.0f / gamma);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, l_fbo.colorTex);

	glEnableVertexAttribArray(RFBO_ATTR_POS);
	glEnableVertexAttribArray(RFBO_ATTR_UV);
#if defined(AURORA_VR)
	/* Позиции квадов глаз — в осях контента, до u_rot: поворот экрана
	   калибровку не ломает. Дисторсия (этап 5) ляжет в UV этих же квадов. */
	glVertexAttribPointer(RFBO_ATTR_POS, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), l_fbo.drawVerts);
	glVertexAttribPointer(RFBO_ATTR_UV, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), l_fbo.drawVerts + 2);
	glDrawArrays(GL_TRIANGLES, 0, l_fbo.drawCount);
#else
	glVertexAttribPointer(RFBO_ATTR_POS, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), l_quadVerts);
	glVertexAttribPointer(RFBO_ATTR_UV, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), l_quadVerts + 2);
	glDrawArrays(GL_TRIANGLES, 0, 6);
#endif
	glDisableVertexAttribArray(RFBO_ATTR_POS);
	glDisableVertexAttribArray(RFBO_ATTR_UV);

	/* Восстановление состояния wrapper'а: его программа (glUseProgram
	   wrapper не выставляет на каждый draw), текстура 0, blend обратно
	   во включённое (кэш 2D ROP на конец кадра: blending on, depth off).
	   Юнит и бинд — через forced-вызовы wrapper'а, НЕ сырым GL: иначе
	   кэш биндингов wrapper'а расходится с реальностью (реально bound 0,
	   в кэше — например conchars), и на чисто-текстовых экранах меню
	   (Start Server и др. до загрузки 3D) cache-hit не выполняет реальный
	   бинд — буквы сэмплируются из текстуры 0 (замороженный кадр заставки). */
	oglwSetCurrentTextureUnitForced(0);
	oglwBindTextureForced(0, 0);
	glUseProgram(oglwGetProgram());
	glEnable(GL_BLEND);
}

void RFBO_GetSize(int *width, int *height)
{
	*width = l_fbo.fboW;
	*height = l_fbo.fboH;
}

void RFBO_SetScale(float scale)
{
	if (scale < 0.25f)
		scale = 0.25f;
	if (scale > 2.0f)
		scale = 2.0f;
	l_fbo.scale = scale;
}

float RFBO_GetScale(void)
{
	/* Пока FBO не готов (до Init, после Shutdown или если Init не удался),
	   рендер идёт напрямую на экран — масштаба нет. */
	if (!l_fbo.ready)
		return 1.0f;
	return l_fbo.scale;
}

void RFBO_SetRotation(int wlOutputTransform)
{
	int newRotation = wlOutputTransform & 3;
	/* Переход 90/270 ↔ 0/180 меняет пропорции буфера (флаг swapped в
	   RFBO_ComputeSize) — FBO надо пересоздать, даже если размер окна не
	   изменился (перенос окна между портретной и ландшафтной панелями).
	   RFBO_Resize сам сравнит вычисленный размер с текущим и пересоздаст. */
	if (l_fbo.ready && ((l_fbo.rotation ^ newRotation) & 1) != 0)
	{
		l_fbo.rotation = newRotation;
		RFBO_Resize(l_fbo.screenW, l_fbo.screenH);
#if defined(AURORA_VR)
		/* Resize мог выйти рано (размер FBO совпал) — ширина контента на
		   экране всё равно сменила ось. */
		RFBO_UpdateLensLayout();
#endif
		return;
	}
#if defined(AURORA_VR)
	if (l_fbo.rotation != newRotation)
	{
		l_fbo.rotation = newRotation;
		RFBO_UpdateLensLayout();
		return;
	}
#endif
	l_fbo.rotation = newRotation;
}

int RFBO_GetRotation(void)
{
	return l_fbo.rotation;
}

void RFBO_TransformTouch(float fx, float fy, int *x, int *y)
{
	/* Палец приходит в координатах окна (композитор Wayland уже учёл
	   buffer transform при доставке ввода). В буфере окна контент повёрнут
	   матрицей квада из вершинного шейдера — для хит-теста применяем
	   обратный поворот: window NDC -> content NDC -> пиксели FBO. */
	float nx = fx * 2.0f - 1.0f;
	float ny = 1.0f - fy * 2.0f;
	float cx, cy;
	switch (l_fbo.rotation & 3)
	{
	case 1:  cx = ny;  cy = -nx; break; /* обратный к (x,y) -> (-y,x) */
	case 2:  cx = -nx; cy = -ny; break;
	case 3:  cx = -ny; cy = nx;  break; /* обратный к (x,y) -> (y,-x) */
	default: cx = nx;  cy = ny;  break;
	}
	*x = (int)((cx + 1.0f) * 0.5f * l_fbo.fboW);
	*y = (int)((1.0f - cy) * 0.5f * l_fbo.fboH);
}

#if defined(AURORA_VR)
void RFBO_SetLensLayout(bool split, float sepMm, float vofsMm, float tiltMm)
{
	l_fbo.lensSplit = split;
	l_fbo.lensSepMm = sepMm;
	l_fbo.lensVofsMm = vofsMm;
	l_fbo.lensTiltMm = tiltMm;
	RFBO_UpdateLensLayout();
}

void RFBO_RefreshDisplayMetrics(void)
{
	RFBO_UpdateLensLayout();
}

bool RFBO_GetLensScreenMm(float *widthMm, float *heightMm)
{
	*widthMm = l_fbo.lensWidthMm;
	*heightMm = l_fbo.lensHeightMm;
	return l_fbo.lensFromDpi;
}
#endif

#else /* !AURORA_FBO — заглушки, сборка без дефайна = движок как раньше. */

bool RFBO_Init(int windowWidth, int windowHeight) { (void)windowWidth; (void)windowHeight; return false; }
void RFBO_Shutdown(void) {}
void RFBO_Resize(int windowWidth, int windowHeight) { (void)windowWidth; (void)windowHeight; }
void RFBO_BindForFrame(void) {}
void RFBO_DrawToScreen(void) {}
void RFBO_GetSize(int *width, int *height) { *width = 0; *height = 0; }
void RFBO_SetScale(float scale) { (void)scale; }
float RFBO_GetScale(void) { return 1.0f; }
void RFBO_SetRotation(int wlOutputTransform) { (void)wlOutputTransform; }
int RFBO_GetRotation(void) { return 0; }
void RFBO_TransformTouch(float fx, float fy, int *x, int *y)
{
	(void)fx; (void)fy; (void)x; (void)y;
}

#endif

#if !(defined(AURORA_FBO) && defined(AURORA_VR))
/* Без FBO или без VR раскладки глаз нет: вывод (если он есть) — один квад. */
void RFBO_SetLensLayout(bool split, float sepMm, float vofsMm, float tiltMm)
{
	(void)split; (void)sepMm; (void)vofsMm; (void)tiltMm;
}
void RFBO_RefreshDisplayMetrics(void) {}
bool RFBO_GetLensScreenMm(float *widthMm, float *heightMm)
{
	*widthMm = 0.0f;
	*heightMm = 0.0f;
	return false;
}
#endif
