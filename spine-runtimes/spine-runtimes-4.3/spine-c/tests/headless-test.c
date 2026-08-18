/******************************************************************************
 * Spine Runtimes License Agreement
 * Last updated April 5, 2025. Replaces all prior versions.
 *
 * Copyright (c) 2013-2025, Esoteric Software LLC
 *
 * Integration of the Spine Runtimes into software or otherwise creating
 * derivative works of the Spine Runtimes is permitted under the terms and
 * conditions of Section 2 of the Spine Editor License Agreement:
 * http://esotericsoftware.com/spine-editor-license
 *
 * Otherwise, it is permitted to integrate the Spine Runtimes into software
 * or otherwise create derivative works of the Spine Runtimes (collectively,
 * "Products"), provided that each user of the Products must obtain their own
 * Spine Editor license and redistribution of the Products in any form must
 * include this license and copyright notice.
 *
 * THE SPINE RUNTIMES ARE PROVIDED BY ESOTERIC SOFTWARE LLC "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL ESOTERIC SOFTWARE LLC BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES,
 * BUSINESS INTERRUPTION, OR LOSS OF USE, DATA, OR PROFITS) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THE SPINE RUNTIMES, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

#include <spine-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <locale.h>

// Custom texture loader that doesn't load actual textures
void *headlessTextureLoader(const char *path) {
	// Don't load actual texture, just return a dummy pointer
	return (void *) 1;
}

void headlessTextureUnloader(void *texture) {
	// Nothing to do
}

// Printer functions
static int indentLevel = 0;
static const char *INDENT = "  ";

void print_indent(const char *format, ...) {
	for (int i = 0; i < indentLevel; i++) {
		printf("%s", INDENT);
	}
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	printf("\n");
}

void indent(void) {
	indentLevel++;
}

void unindent(void) {
	indentLevel--;
}

void printSkeletonData(spine_skeleton_data data) {
	print_indent("SkeletonData {");
	indent();

	const char *name = spine_skeleton_data_get_name(data);
	const char *version = spine_skeleton_data_get_version(data);
	const char *hash = spine_skeleton_data_get_hash(data);
	const char *imagesPath = spine_skeleton_data_get_images_path(data);
	const char *audioPath = spine_skeleton_data_get_audio_path(data);

	print_indent("name: \"%s\"", name ? name : "");
	print_indent("version: \"%s\"", version ? version : "");
	print_indent("hash: \"%s\"", hash ? hash : "");
	print_indent("x: %.6f", spine_skeleton_data_get_x(data));
	print_indent("y: %.6f", spine_skeleton_data_get_y(data));
	print_indent("width: %.6f", spine_skeleton_data_get_width(data));
	print_indent("height: %.6f", spine_skeleton_data_get_height(data));
	print_indent("referenceScale: %.6f", spine_skeleton_data_get_reference_scale(data));
	print_indent("fps: %.6f", spine_skeleton_data_get_fps(data));
	print_indent("imagesPath: \"%s\"", imagesPath ? imagesPath : "");
	print_indent("audioPath: \"%s\"", audioPath ? audioPath : "");

	// TODO: Add bones, slots, skins, animations, etc. in future expansion

	unindent();
	print_indent("}");
}

void printSkeleton(spine_skeleton skeleton) {
	print_indent("Skeleton {");
	indent();

	print_indent("x: %.6f", spine_skeleton_get_x(skeleton));
	print_indent("y: %.6f", spine_skeleton_get_y(skeleton));
	print_indent("scaleX: %.6f", spine_skeleton_get_scale_x(skeleton));
	print_indent("scaleY: %.6f", spine_skeleton_get_scale_y(skeleton));
	print_indent("time: %.6f", spine_skeleton_get_time(skeleton));

	// TODO: Add runtime state (bones, slots, etc.) in future expansion

	unindent();
	print_indent("}");
}

// Helper function to read file contents
uint8_t *read_file(const char *path, int *length) {
	FILE *file = fopen(path, "rb");
	if (!file) return NULL;

	fseek(file, 0, SEEK_END);
	*length = (int) ftell(file);
	fseek(file, 0, SEEK_SET);

	uint8_t *data = (uint8_t *) malloc(*length + 1);
	fread(data, 1, *length, file);
	data[*length] = '\0';
	fclose(file);

	return data;
}

int main(int argc, char *argv[]) {
	// Set locale to ensure consistent number formatting
	setlocale(LC_ALL, "C");

	if (argc < 3) {
		fprintf(stderr, "Usage: DebugPrinter <skeleton-path> <atlas-path> [animation-name]\n");
		return 1;
	}

	spine_bone_set_y_down(false);

	const char *skeletonPath = argv[1];
	const char *atlasPath = argv[2];
	const char *animationName = argc >= 4 ? argv[3] : NULL;

	// Read atlas file
	int atlasLength = 0;
	uint8_t *atlasBytes = read_file(atlasPath, &atlasLength);
	if (!atlasBytes) {
		fprintf(stderr, "Failed to read atlas file\n");
		return 1;
	}

	// Extract directory from atlas path for texture loading
	char atlasDir[1024] = "";
	const char *lastSlash = strrchr(atlasPath, '/');
	if (lastSlash) {
		int dirLen = lastSlash - atlasPath + 1;
		strncpy(atlasDir, atlasPath, dirLen);
		atlasDir[dirLen] = '\0';
	}

	// Load atlas with headless texture loader
	spine_atlas_result atlasResult = spine_atlas_load_callback((const char *) atlasBytes, atlasDir, headlessTextureLoader, headlessTextureUnloader);
	free(atlasBytes);

	const char *atlasError = spine_atlas_result_get_error(atlasResult);
	if (atlasError) {
		fprintf(stderr, "Failed to load atlas: %s\n", atlasError);
		spine_atlas_result_dispose(atlasResult);
		return 1;
	}

	spine_atlas atlas = spine_atlas_result_get_atlas(atlasResult);

	// Load skeleton data
	spine_skeleton_data_result result = {0};
	spine_skeleton_data skeletonData = NULL;

	if (strstr(skeletonPath, ".json") != NULL) {
		int skeletonLength = 0;
		uint8_t *skeletonBytes = read_file(skeletonPath, &skeletonLength);
		if (!skeletonBytes) {
			fprintf(stderr, "Failed to read skeleton file\n");
			spine_atlas_result_dispose(atlasResult);
			return 1;
		}
		result = spine_skeleton_data_load_json(atlas, (const char *) skeletonBytes, skeletonPath);
		free(skeletonBytes);
	} else {
		int skeletonLength = 0;
		uint8_t *skeletonBytes = read_file(skeletonPath, &skeletonLength);
		if (!skeletonBytes) {
			fprintf(stderr, "Failed to read skeleton file\n");
			spine_atlas_result_dispose(atlasResult);
			return 1;
		}
		result = spine_skeleton_data_load_binary(atlas, skeletonBytes, skeletonLength, skeletonPath);
		free(skeletonBytes);
	}

	skeletonData = spine_skeleton_data_result_get_data(result);
	if (!skeletonData) {
		const char *error = spine_skeleton_data_result_get_error(result);
		fprintf(stderr, "Failed to load skeleton data: %s\n", error ? error : "Unknown error");
		spine_skeleton_data_result_dispose(result);
		spine_atlas_result_dispose(atlasResult);
		return 1;
	}

	// Print skeleton data
	printf("=== SKELETON DATA ===\n");
	printSkeletonData(skeletonData);

	// Create skeleton instance
	spine_skeleton skeleton = spine_skeleton_create(skeletonData);

	// Create animation state
	spine_animation_state_data stateData = spine_animation_state_data_create(skeletonData);
	spine_animation_state state = spine_animation_state_create(stateData);

	spine_skeleton_setup_pose(skeleton);

	// Set animation or setup pose
	if (animationName != NULL) {
		// Find and set animation
		spine_animation animation = spine_skeleton_data_find_animation(skeletonData, animationName);
		if (!animation) {
			fprintf(stderr, "Animation not found: %s\n", animationName);
			spine_animation_state_dispose(state);
			spine_animation_state_data_dispose(stateData);
			spine_skeleton_dispose(skeleton);
			spine_skeleton_data_result_dispose(result);
			spine_atlas_result_dispose(atlasResult);
			return 1;
		}
		spine_animation_state_set_animation_1(state, 0, animationName, 1);
		// Update and apply
		spine_animation_state_update(state, 0.016f);
		spine_animation_state_apply(state, skeleton);
	}

	spine_skeleton_update_world_transform(skeleton, SPINE_PHYSICS_UPDATE);

	// Print skeleton state
	printf("\n=== SKELETON STATE ===\n");
	printSkeleton(skeleton);

	// Cleanup
	spine_animation_state_dispose(state);
	spine_animation_state_data_dispose(stateData);
	spine_skeleton_dispose(skeleton);
	spine_skeleton_data_result_dispose(result);
	spine_atlas_result_dispose(atlasResult);

	return 0;
}
