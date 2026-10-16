// D3D shader-model-1..3 bytecode -> GLSL ES 3.00 via vkd3d-shader (-> SPIR-V) and SPIRV-Cross, plus the semantic tables
// (which register carries which usage) that the browser needs to link vertex outputs to pixel inputs and bind vertex streams.
#include "emu.h"
#include <vkd3d_shader.h>
#include <spirv_cross/spirv_cross_c.h>

static void parse_dcl(const uint8_t *code, uint32_t len, ShaderXlat *x) {
    const uint32_t *t = (const uint32_t *)code; uint32_t n = len / 4;
    x->is_ps = (t[0] >> 16) == 0xffff; x->version = t[0] & 0xffff;
    for (uint32_t i = 1; i < n;) {
        uint32_t tok = t[i];
        if (tok == 0x0000ffff) break;
        if ((tok & 0xffff) == 0xfffe) { i += 1 + ((tok >> 16) & 0x7fff); continue; }
        uint32_t ops = (tok >> 24) & 0xf;
        if ((tok & 0xffff) == 0x1f && i + 2 < n) {                                       // DCL
            uint32_t u = t[i + 1], d = t[i + 2], type = ((d >> 28) & 7) | ((d >> 8) & 0x18), reg = d & 0x7ff;
            uint32_t usage = u & 0x1f, idx = (u >> 16) & 0xf, texty = (u >> 27) & 0xf;
            IOEnt e = {(uint8_t)reg, (uint8_t)usage, (uint8_t)idx, (uint8_t)texty};
            if (type == 10) { if (x->nsamp < 16) { x->samp[x->nsamp++] = e; } }          // D3DSPR_SAMPLER
            else if (type == 1 && x->nin < 32) { x->in[x->nin++] = e; }                  // INPUT
            else if (type == 3 && x->nin < 32) { e.usage = 5; e.idx = (uint8_t)reg; x->in[x->nin++] = e; }   // ps_2_x `dcl t#` (texcoord)
            else if (type == 6 && x->nout < 32) { x->out[x->nout++] = e; }               // OUTPUT (vs_3_0)
            else if (type == 5 && x->nout < 32) { e.usage = 5; e.idx = (uint8_t)reg; x->out[x->nout++] = e; } // vs_2: texcoord output
        }
        i += 1 + ops;
    }
}

// returns 0 on success; x->glsl is malloc'd
int shader_translate(const uint8_t *code, uint32_t len, ShaderXlat *x) {
    memset(x, 0, sizeof *x); parse_dcl(code, len, x);
    struct vkd3d_shader_compile_info info = { VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO, NULL, {code, len}, VKD3D_SHADER_SOURCE_D3D_BYTECODE, VKD3D_SHADER_TARGET_SPIRV_BINARY };
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    struct vkd3d_shader_code spv = {0}; char *msgs = NULL;
    int rc = vkd3d_shader_compile(&info, &spv, &msgs);
    if (rc < 0) { fprintf(stderr, "[shader] vkd3d failed: %s\n", msgs ? msgs : "?"); if (msgs) vkd3d_shader_free_messages(msgs); return -1; }
    if (msgs) vkd3d_shader_free_messages(msgs);
    spvc_context ctx; spvc_parsed_ir ir; spvc_compiler comp; spvc_compiler_options opts; const char *src = NULL;
    spvc_context_create(&ctx);
    if (spvc_context_parse_spirv(ctx, (const SpvId *)spv.code, spv.size / 4, &ir) != SPVC_SUCCESS ||
        spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) != SPVC_SUCCESS) {
        fprintf(stderr, "[shader] spirv-cross parse failed: %s\n", spvc_context_get_last_error_string(ctx)); spvc_context_destroy(ctx); vkd3d_shader_free_shader_code(&spv); return -1;
    }
    spvc_compiler_create_compiler_options(comp, &opts);
    spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_GLSL_VERSION, 300);
    spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE);
    spvc_compiler_install_compiler_options(comp, opts);
    spvc_compiler_build_combined_image_samplers(comp);
    { const spvc_combined_image_sampler *cs = NULL; size_t ncs = 0;       // the C API leaves combined samplers unnamed: use the CLI's SPIRV_Cross_Combined<texture><sampler> names
      spvc_compiler_get_combined_image_samplers(comp, &cs, &ncs);
      for (size_t i = 0; i < ncs; i++) { const char *in = spvc_compiler_get_name(comp, cs[i].image_id), *sn = spvc_compiler_get_name(comp, cs[i].sampler_id); char nm[160]; snprintf(nm, sizeof nm, "SPIRV_Cross_Combined%s%s", in ? in : "t", sn ? sn : "s"); spvc_compiler_set_name(comp, cs[i].combined_id, nm); } }
    if (spvc_compiler_compile(comp, &src) != SPVC_SUCCESS) { fprintf(stderr, "[shader] spirv-cross compile failed: %s\n", spvc_context_get_last_error_string(ctx)); spvc_context_destroy(ctx); vkd3d_shader_free_shader_code(&spv); return -1; }
    x->glsl = strdup(src); x->glsl_len = (uint32_t)strlen(src);
    spvc_context_destroy(ctx); vkd3d_shader_free_shader_code(&spv); return 0;
}
