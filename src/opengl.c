#include "third_party/gl_core/gl_core_3_1.h"
#include <SDL.h>
#include <stdio.h>
#include <stdbool.h>
#include "types.h"
#include "util.h"
#include "glsl_shader.h"
#include "config.h"
#include "hd_menu.h"
#include "hd_fx.h"

#define CODE(...) #__VA_ARGS__

static SDL_Window *g_window;
static uint8 *g_screen_buffer;
static size_t g_screen_buffer_size;
static int g_draw_width, g_draw_height;
static unsigned int g_program, g_VAO, g_VBO;
static GlTextureWithSize g_texture;
static GlslShader *g_glsl_shader;
static bool g_opengl_es;
static unsigned int g_overlay_texture;
static uint32 *g_overlay_pixels;
static int g_overlay_cap;

static void GL_APIENTRY MessageCallback(GLenum source,
                GLenum type,
                GLuint id,
                GLenum severity,
                GLsizei length,
                const GLchar *message,
                const void *userParam) {
  if (type == GL_DEBUG_TYPE_OTHER)
    return;

  fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
          (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""),
          type, severity, message);
  if (type == GL_DEBUG_TYPE_ERROR)
    Die("OpenGL error!\n");
}

static bool OpenGLRenderer_Init(SDL_Window *window) {
  g_window = window;
  SDL_GLContext context = SDL_GL_CreateContext(window);
  (void)context;

  SDL_GL_SetSwapInterval(1);
  ogl_LoadFunctions();

  if (!g_opengl_es) {
    if (!ogl_IsVersionGEQ(3, 3))
      Die("You need OpenGL 3.3");
  } else {
    int majorVersion = 0, minorVersion = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &majorVersion);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minorVersion);
    if (majorVersion < 3)
      Die("You need OpenGL ES 3.0");

  }

  if (kDebugFlag) {
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(MessageCallback, 0);
  }

  glGenTextures(1, &g_texture.gl_texture);

  static const float kVertices[] = {
    // positions          // texture coords
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f, // top left
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f, // bottom left
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f, // top right
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f,  // bottom right
  };

  // create a vertex buffer object
  glGenBuffers(1, &g_VBO);

  // vertex array object
  glGenVertexArrays(1, &g_VAO);
  // 1. bind Vertex Array Object
  glBindVertexArray(g_VAO);
  // 2. copy our vertices array in a buffer for OpenGL to use
  glBindBuffer(GL_ARRAY_BUFFER, g_VBO);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
  // position attribute
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(0);
  // texture coord attribute
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  // vertex shader
  const GLchar *vs_code_core = "#version 330 core\n" CODE(
  layout(location = 0) in vec3 aPos;
  layout(location = 1) in vec2 aTexCoord;
  out vec2 TexCoord;
  void main() {
    gl_Position = vec4(aPos, 1.0);
    TexCoord = vec2(aTexCoord.x, aTexCoord.y);
  }
);

  const GLchar *vs_code_es = "#version 300 es\n" CODE(
  layout(location = 0) in vec3 aPos;
  layout(location = 1) in vec2 aTexCoord;
  out vec2 TexCoord;
  void main() {
    gl_Position = vec4(aPos, 1.0);
    TexCoord = vec2(aTexCoord.x, aTexCoord.y);
  }
);

  const GLchar *vs_code = g_opengl_es ? vs_code_es : vs_code_core;
  unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_code, NULL);
  glCompileShader(vs);

  int success;
  char infolog[512];
  glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(vs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // fragment shader
  const GLchar *fs_code_core = "#version 330 core\n" CODE(
  out vec4 FragColor;
  in vec2 TexCoord;
  // texture samplers
  uniform sampler2D texture1;
  void main() {
    FragColor = texture(texture1, TexCoord);
  }
);

  const GLchar *fs_code_es = "#version 300 es\n" CODE(
  precision mediump float;
  out vec4 FragColor;
  in vec2 TexCoord;
  // texture samplers
  uniform sampler2D texture1;
  void main() {
    FragColor = texture(texture1, TexCoord);
  }
);


  const GLchar *fs_code = g_opengl_es ? fs_code_es : fs_code_core;
  unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_code, NULL);
  glCompileShader(fs);

  glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(fs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // create program
  int program = g_program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glGetProgramiv(program, GL_LINK_STATUS, &success);

  if (!success) {
    glGetProgramInfoLog(program, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  if (g_config.shader)
    g_glsl_shader = GlslShader_CreateFromFile(g_config.shader, g_opengl_es);
  
  return true;
}

static void OpenGLRenderer_Destroy() {
}

static void OpenGLRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  int size = width * height;

  if (size > g_screen_buffer_size) {
    g_screen_buffer_size = size;
    free(g_screen_buffer);
    g_screen_buffer = malloc(size * 4);
  }

  g_draw_width = width;
  g_draw_height = height;
  *pixels = g_screen_buffer;
  *pitch = width * 4;
}

// Disegna il rettangolo con g_program. I vertici si reimpostano ogni volta:
// lo shader GLSL usa lo stesso oggetto dei vertici, cambia i puntatori e alla
// fine disattiva gli attributi (senza questo il disegno uscirebbe nero)
static void DrawQuad() {
  glUseProgram(g_program);
  glBindVertexArray(g_VAO);
  glBindBuffer(GL_ARRAY_BUFFER, g_VBO);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void OpenGLRenderer_EndDraw() {
  int drawable_width, drawable_height;

  SDL_GL_GetDrawableSize(g_window, &drawable_width, &drawable_height);
  
  int viewport_width = drawable_width, viewport_height = drawable_height;

  if (!g_config.ignore_aspect_ratio) {
    if (viewport_width * g_draw_height < viewport_height * g_draw_width)
      viewport_height = viewport_width * g_draw_height / g_draw_width;  // limit height
    else
      viewport_width = viewport_height * g_draw_width / g_draw_height;  // limit width
  }

  int viewport_x = (drawable_width - viewport_width) >> 1;
  int viewport_y = (drawable_height - viewport_height) >> 1;

  glBindTexture(GL_TEXTURE_2D, g_texture.gl_texture);
  if (g_draw_width == g_texture.width && g_draw_height == g_texture.height) {
    if (!g_opengl_es)
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_screen_buffer);
    else
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height, GL_BGRA, GL_UNSIGNED_BYTE, g_screen_buffer);
  } else {
    g_texture.width = g_draw_width;
    g_texture.height = g_draw_height;
    if (!g_opengl_es)
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_draw_width, g_draw_height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_screen_buffer);
    else
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_draw_width, g_draw_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, g_screen_buffer);
  }

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  // Con il gioco sfocato dietro inventario e menu lo shader non si applica:
  // torna appena la sfocatura finisce. Neanche nella nebbia del bosco
  if (g_glsl_shader == NULL || HdFx_MenuBlurActive() || HdFx_ForestFogScene()) {
    glViewport(viewport_x, viewport_y, viewport_width, viewport_height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_texture.gl_texture);
    int filter = g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    DrawQuad();
  } else {
    GlslShader_Render(g_glsl_shader, &g_texture, viewport_x, viewport_y, viewport_width, viewport_height);
  }

  // Testo nitido: il riquadro dei dialoghi viene ridisegnato sopra, ingrandito
  // 4x con un algoritmo per pixel art e senza passare dallo shader.
  int rx, ry, rw, rh;
  if (HdText_GetRect(g_draw_width, g_draw_height, &rx, &ry, &rw, &rh)) {
    if (rw * rh * 16 > g_overlay_cap) {
      g_overlay_cap = rw * rh * 16;
      free(g_overlay_pixels);
      g_overlay_pixels = malloc(g_overlay_cap * 4);
    }
    HdText_Scale4x(g_screen_buffer, g_draw_width * 4, rx, ry, rw, rh, g_overlay_pixels);
    if (!g_overlay_texture)
      glGenTextures(1, &g_overlay_texture);
    glBindTexture(GL_TEXTURE_2D, g_overlay_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw * 4, rh * 4, 0, GL_BGRA,
                 g_opengl_es ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT_8_8_8_8_REV, g_overlay_pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    int x0 = viewport_x + rx * viewport_width / g_draw_width;
    int x1 = viewport_x + (rx + rw) * viewport_width / g_draw_width;
    int y0 = viewport_y + (g_draw_height - ry - rh) * viewport_height / g_draw_height;
    int y1 = viewport_y + (g_draw_height - ry) * viewport_height / g_draw_height;
    glViewport(x0, y0, x1 - x0, y1 - y0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_overlay_texture);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // i trasparenti sono neri: colori premoltiplicati
    DrawQuad();
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, g_texture.gl_texture);
  }

  SDL_GL_SwapWindow(g_window);
}

static const struct RendererFuncs kOpenGLRendererFuncs = {
  &OpenGLRenderer_Init,
  &OpenGLRenderer_Destroy,
  &OpenGLRenderer_BeginDraw,
  &OpenGLRenderer_EndDraw,
};

// Shader GLSL caricato (per esempio Four Swords)
bool OpenGLRenderer_HasShader(void) {
  return g_glsl_shader != NULL;
}

void OpenGLRenderer_Create(struct RendererFuncs *funcs, bool use_opengl_es) {
  g_opengl_es = use_opengl_es;
  if (!g_opengl_es) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  } else {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  }
  *funcs = kOpenGLRendererFuncs;
}

