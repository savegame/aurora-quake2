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
	int screenW, screenH; /* реальный размер окна */
	int fboW, fboH;       /* размер буфера рендеринга */
	float scale;
	int rotation;         /* enum wl_output_transform */
	bool ready;
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
	"void main()\n"
	"{\n"
	"	gl_FragColor = texture2D(u_tex, v_uv);\n"
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
	glBindTexture(GL_TEXTURE_2D, 0);

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
}

bool RFBO_Init(int windowWidth, int windowHeight)
{
	/* rotation выставляется вызывающим кодом ДО Init (от него зависят
	   размеры FBO) — сохраняем его. */
	int rotation = l_fbo.rotation;
	memset(&l_fbo, 0, sizeof(l_fbo));
	l_fbo.scale = 1.0f;
	l_fbo.rotation = rotation;

	l_fbo.program = RFBO_CreateProgram();
	if (l_fbo.program == 0)
		return false;
	l_fbo.uRot = glGetUniformLocation(l_fbo.program, "u_rot");
	l_fbo.uTex = glGetUniformLocation(l_fbo.program, "u_tex");

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
	R_printf(PRINT_ALL, "RFBO: %ix%i -> screen %ix%i\n", l_fbo.fboW, l_fbo.fboH, l_fbo.screenW, l_fbo.screenH);
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
	if (windowWidth == l_fbo.screenW && windowHeight == l_fbo.screenH)
		return;

	l_fbo.screenW = windowWidth;
	l_fbo.screenH = windowHeight;
	RFBO_ComputeSize(windowWidth, windowHeight, &l_fbo.fboW, &l_fbo.fboH);

	RFBO_DestroyTargets();
	if (!RFBO_CreateTargets(l_fbo.fboW, l_fbo.fboH))
	{
		RFBO_Shutdown();
		return;
	}

	viddef.width = l_fbo.fboW;
	viddef.height = l_fbo.fboH;
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

	glUseProgram(l_fbo.program);
	glUniformMatrix2fv(l_fbo.uRot, 1, GL_FALSE, l_rotMatrices[l_fbo.rotation & 3]);
	glUniform1i(l_fbo.uTex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, l_fbo.colorTex);

	glEnableVertexAttribArray(RFBO_ATTR_POS);
	glEnableVertexAttribArray(RFBO_ATTR_UV);
	glVertexAttribPointer(RFBO_ATTR_POS, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), l_quadVerts);
	glVertexAttribPointer(RFBO_ATTR_UV, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), l_quadVerts + 2);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(RFBO_ATTR_POS);
	glDisableVertexAttribArray(RFBO_ATTR_UV);

	/* Восстановление состояния wrapper'а: его программа (glUseProgram
	   wrapper не выставляет на каждый draw), текстура 0, blend обратно
	   во включённое (кэш 2D ROP на конец кадра: blending on, depth off). */
	glBindTexture(GL_TEXTURE_2D, 0);
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
	return l_fbo.scale;
}

void RFBO_SetRotation(int wlOutputTransform)
{
	l_fbo.rotation = wlOutputTransform & 3;
}

int RFBO_GetRotation(void)
{
	return l_fbo.rotation;
}

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

#endif
