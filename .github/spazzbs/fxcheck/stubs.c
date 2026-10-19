#include <stddef.h>
#include <stdbool.h>
/* graphics calls only used by ep_compile, which the harness never runs */
#define STUB(name) void *name() { return NULL; }
STUB(gs_vertexshader_create) STUB(gs_pixelshader_create) STUB(gs_shader_destroy)
STUB(gs_shader_get_num_params) STUB(gs_shader_get_param_by_idx) STUB(gs_shader_get_param_info)
STUB(gs_shader_get_viewproj_matrix) STUB(gs_shader_get_world_matrix)
STUB(gs_samplerstate_create) STUB(gs_preprocessor_name)
STUB(gs_effect_get_param_by_name) STUB(gs_effect_get_annotation_by_name)
STUB(gs_shader_get_param_by_name)
STUB(obs_get_video_info) STUB(uuid_generate) STUB(uuid_unparse_lower)
