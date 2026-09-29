R"(
#version 130

uniform usampler2D indexed_text;
uniform sampler1D  palette_text;
uniform int transparent_flag;

void main() {
	uint palette_index = texture(indexed_text, gl_TexCoord[0].st).r;
	if(transparent_flag == 1 && palette_index == 0u) {
		gl_FragColor = vec4(0,0,0,0);
	}
	else {
		// Sample texel centers: index 0 -> 0.5/256, index 255 -> 255.5/256
		vec4 palette_color = texture(palette_text, (float(palette_index) + 0.5) / 256.0);
		gl_FragColor = vec4(palette_color.rgb, 1);
	}
}
)"
