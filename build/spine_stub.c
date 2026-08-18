/* Mandatory host-implemented callbacks for spine-c.
 * We do NOT create GPU textures in the C layer - the atlas PNG images are
 * loaded on the Python/renpy side. These stubs only need to keep the atlas
 * parsing (UV rects etc.) working. */
#include <spine/extension.h>
#include <stdio.h>

void _spAtlasPage_createTexture(spAtlasPage *self, const char *path) {
	/* 不在 C 层创建 GPU 纹理；但必须保留 atlas 解析出的 width/height，
	 * atlas region 的 uv 计算依赖 page->width / page->height（Atlas.c 中
	 * region->u = x / page->width），清零会导致 uv 除零变成 inf。 */
	self->rendererObject = 0;
}

void _spAtlasPage_disposeTexture(spAtlasPage *self) {
}

char *_spUtil_readFile(const char *path, int *length) {
	char *data;
	size_t result;
	FILE *file = fopen(path, "rb");
	if (!file) return 0;
	fseek(file, 0, SEEK_END);
	*length = (int) ftell(file);
	fseek(file, 0, SEEK_SET);
	data = MALLOC(char, *length);
	result = fread(data, 1, *length, file);
	(void) result;
	fclose(file);
	return data;
}
