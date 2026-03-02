/* Dynamic glslang C API loader for the Windows VkFFT backend.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include "sox_i.h"

#include <glslang/Include/glslang_c_interface.h>
#include <windows.h>

static HMODULE module;

static int (*initialize_process_impl)(void);
static void (*finalize_process_impl)(void);
static glslang_shader_t *(*shader_create_impl)(
    glslang_input_t const *);
static void (*shader_delete_impl)(glslang_shader_t *);
static int (*shader_preprocess_impl)(
    glslang_shader_t *, glslang_input_t const *);
static int (*shader_parse_impl)(
    glslang_shader_t *, glslang_input_t const *);
static char const *(*shader_get_info_log_impl)(glslang_shader_t *);
static glslang_program_t *(*program_create_impl)(void);
static void (*program_delete_impl)(glslang_program_t *);
static void (*program_add_shader_impl)(
    glslang_program_t *, glslang_shader_t *);
static int (*program_link_impl)(glslang_program_t *, int);
static char const *(*program_get_info_log_impl)(glslang_program_t *);
static void (*program_spirv_generate_impl)(
    glslang_program_t *, glslang_stage_t);
static size_t (*program_spirv_get_size_impl)(glslang_program_t *);
static unsigned int *(*program_spirv_get_ptr_impl)(glslang_program_t *);
static char const *(*program_spirv_get_messages_impl)(
    glslang_program_t *);

static void clear_functions(void)
{
  initialize_process_impl = NULL;
  finalize_process_impl = NULL;
  shader_create_impl = NULL;
  shader_delete_impl = NULL;
  shader_preprocess_impl = NULL;
  shader_parse_impl = NULL;
  shader_get_info_log_impl = NULL;
  program_create_impl = NULL;
  program_delete_impl = NULL;
  program_add_shader_impl = NULL;
  program_link_impl = NULL;
  program_get_info_log_impl = NULL;
  program_spirv_generate_impl = NULL;
  program_spirv_get_size_impl = NULL;
  program_spirv_get_ptr_impl = NULL;
  program_spirv_get_messages_impl = NULL;
}

static FARPROC load_function(char const *name)
{
  FARPROC function = GetProcAddress(module, name);

  if (!function)
    lsx_fail("glslang.dll does not export %s", name);
  return function;
}

#define LOAD_FUNCTION(variable, name) \
  variable##_impl = (void *)load_function("glslang_" #name)

static int load_functions(void)
{
  module = LoadLibraryA("glslang.dll");
  if (!module) {
    lsx_fail(
        "failed to load glslang.dll (Windows error %lu)",
        (unsigned long)GetLastError());
    return 0;
  }
  LOAD_FUNCTION(initialize_process, initialize_process);
  LOAD_FUNCTION(finalize_process, finalize_process);
  LOAD_FUNCTION(shader_create, shader_create);
  LOAD_FUNCTION(shader_delete, shader_delete);
  LOAD_FUNCTION(shader_preprocess, shader_preprocess);
  LOAD_FUNCTION(shader_parse, shader_parse);
  LOAD_FUNCTION(shader_get_info_log, shader_get_info_log);
  LOAD_FUNCTION(program_create, program_create);
  LOAD_FUNCTION(program_delete, program_delete);
  LOAD_FUNCTION(program_add_shader, program_add_shader);
  LOAD_FUNCTION(program_link, program_link);
  LOAD_FUNCTION(program_get_info_log, program_get_info_log);
  LOAD_FUNCTION(program_spirv_generate, program_SPIRV_generate);
  LOAD_FUNCTION(program_spirv_get_size, program_SPIRV_get_size);
  LOAD_FUNCTION(program_spirv_get_ptr, program_SPIRV_get_ptr);
  LOAD_FUNCTION(program_spirv_get_messages, program_SPIRV_get_messages);
  if (!initialize_process_impl || !finalize_process_impl ||
      !shader_create_impl || !shader_delete_impl ||
      !shader_preprocess_impl || !shader_parse_impl ||
      !shader_get_info_log_impl || !program_create_impl ||
      !program_delete_impl || !program_add_shader_impl ||
      !program_link_impl || !program_get_info_log_impl ||
      !program_spirv_generate_impl ||
      !program_spirv_get_size_impl ||
      !program_spirv_get_ptr_impl ||
      !program_spirv_get_messages_impl) {
    FreeLibrary(module);
    module = NULL;
    clear_functions();
    return 0;
  }
  return 1;
}

int glslang_initialize_process(void)
{
  if (!module && !load_functions())
    return 0;
  if (!initialize_process_impl()) {
    FreeLibrary(module);
    module = NULL;
    clear_functions();
    return 0;
  }
  return 1;
}

void glslang_finalize_process(void)
{
  if (!module)
    return;
  finalize_process_impl();
  FreeLibrary(module);
  module = NULL;
  clear_functions();
}

glslang_shader_t *glslang_shader_create(
    glslang_input_t const *input)
{
  return shader_create_impl(input);
}

void glslang_shader_delete(glslang_shader_t *shader)
{
  shader_delete_impl(shader);
}

int glslang_shader_preprocess(
    glslang_shader_t *shader, glslang_input_t const *input)
{
  return shader_preprocess_impl(shader, input);
}

int glslang_shader_parse(
    glslang_shader_t *shader, glslang_input_t const *input)
{
  return shader_parse_impl(shader, input);
}

char const *glslang_shader_get_info_log(glslang_shader_t *shader)
{
  return shader_get_info_log_impl(shader);
}

glslang_program_t *glslang_program_create(void)
{
  return program_create_impl();
}

void glslang_program_delete(glslang_program_t *program)
{
  program_delete_impl(program);
}

void glslang_program_add_shader(
    glslang_program_t *program, glslang_shader_t *shader)
{
  program_add_shader_impl(program, shader);
}

int glslang_program_link(glslang_program_t *program, int messages)
{
  return program_link_impl(program, messages);
}

char const *glslang_program_get_info_log(
    glslang_program_t *program)
{
  return program_get_info_log_impl(program);
}

void glslang_program_SPIRV_generate(
    glslang_program_t *program, glslang_stage_t stage)
{
  program_spirv_generate_impl(program, stage);
}

size_t glslang_program_SPIRV_get_size(glslang_program_t *program)
{
  return program_spirv_get_size_impl(program);
}

unsigned int *glslang_program_SPIRV_get_ptr(
    glslang_program_t *program)
{
  return program_spirv_get_ptr_impl(program);
}

char const *glslang_program_SPIRV_get_messages(
    glslang_program_t *program)
{
  return program_spirv_get_messages_impl(program);
}
