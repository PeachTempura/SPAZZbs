/* SPAZZbs CI: turns an OBS .effect into the exact HLSL OBS hands to
 * D3DCompile, so the Windows job can check it with fxc before building. */
/* Parse an OBS .effect with OBS's real parser and dump the HLSL each pass
 * would hand to D3DCompile (after the d3d11 token replacements). */
#include "graphics/effect-parser.c"
#include <stdio.h>

static void d3dify(const char *in, struct dstr *out)
{
	static const char *from[] = {"POSITION", "TARGET", "texture2d", "texture3d", "texture_cube", "sampler_state", "VERTEXID", NULL};
	static const char *to[] = {"SV_Position", "SV_Target", "Texture2D", "Texture3D", "TextureCube", "SamplerState", "SV_VertexID"};
	dstr_copy(out, "static const bool obs_glsl_compile = false;\n\n");
	const char *p = in;
	while (*p) {
		if (isalpha((unsigned char)*p) || *p == '_') {
			const char *s = p;
			while (isalnum((unsigned char)*p) || *p == '_') p++;
			size_t len = p - s;
			int hit = -1;
			for (int i = 0; from[i]; i++)
				if (strlen(from[i]) == len && strncmp(from[i], s, len) == 0) hit = i;
			if (hit >= 0) dstr_cat(out, to[hit]); else dstr_ncat(out, s, len);
		} else {
			dstr_ncat(out, p, 1);
			p++;
		}
	}
}

int main(int argc, char **argv)
{
	if (argc < 3) return 2;
	char *src = os_quick_read_utf8_file(argv[1]);
	if (!src) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }

	struct effect_parser ep;
	ep_init(&ep);

	/* Same as ep_parse(), minus the GPU compile step. */
	struct cf_def def;
	cf_def_init(&def);
	def.name.str.array = "_D3D11";
	def.name.str.len = strlen("_D3D11");
	strref_copy(&def.name.unmerged_str, &def.name.str);
	cf_preprocessor_add_def(&ep.cfp.pp, &def);

	bool ok = cf_parser_parse(&ep.cfp, src, argv[1]);
	while (ok && ep.cfp.cur_token && ep.cfp.cur_token->type != CFTOKEN_NONE) {
		if (cf_token_is(&ep.cfp, ";") || is_whitespace(*ep.cfp.cur_token->str.array)) {
			ep.cfp.cur_token++;
		} else if (cf_token_is(&ep.cfp, "struct")) {
			ep_parse_struct(&ep);
		} else if (cf_token_is(&ep.cfp, "technique")) {
			ep_parse_technique(&ep);
		} else if (cf_token_is(&ep.cfp, "sampler_state")) {
			ep_parse_sampler_state(&ep);
		} else if (cf_token_is(&ep.cfp, "{")) {
			cf_adderror(&ep.cfp, "Unexpected code segment", LEX_ERROR, NULL, NULL, NULL);
			cf_pass_pair(&ep.cfp, '{', '}');
		} else {
			ep_parse_other(&ep);
		}
	}
	char *errs = error_data_buildstring(&ep.cfp.error_list);
	if (errs && *errs) { fprintf(stderr, "PARSER MESSAGES:\n%s\n", errs); }
	if (!ok || error_data_has_errors(&ep.cfp.error_list)) { fprintf(stderr, "PARSE FAILED\n"); return 1; }

	int count = 0;
	for (size_t t = 0; t < ep.techniques.num; t++) {
		struct ep_technique *tech = ep.techniques.array + t;
		for (size_t pi = 0; pi < tech->passes.num; pi++) {
			struct ep_pass *pass = tech->passes.array + pi;
			for (int stage = 0; stage < 2; stage++) {
				struct dstr shader = {0}, out = {0};
				dstr_array_t used = {0};
				ep_makeshaderstring(&ep, &shader, stage == 0 ? &pass->vertex_program : &pass->fragment_program, &used);
				d3dify(shader.array ? shader.array : "", &out);
				char path[512];
				snprintf(path, sizeof(path), "%s/%s_p%zu_%s.hlsl", argv[2], tech->name, pi, stage == 0 ? "vs" : "ps");
				FILE *f = fopen(path, "wb");
				fwrite(out.array, 1, out.len, f);
				fclose(f);
				printf("%s\n", path);
				count++;
			}
		}
	}
	return count ? 0 : 1;
}
