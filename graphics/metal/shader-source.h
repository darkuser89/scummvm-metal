/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_SHADER_SOURCE_H
#define GRAPHICS_METAL_SHADER_SOURCE_H

#include "common/config-manager.h"
#include "common/array.h"
#include "common/fs.h"
#include "common/stream.h"
#include "common/textconsole.h"

namespace Graphics { namespace Metal {

// Opt-in native extensions retain the engine's vertex/uniform ABI and entry
// names. They never interpret a GLSL file as MSL or fall back to GL commands.
// Known catalog entries remain the default when no override file exists.
inline Common::String shaderOverride(const char *engine, const char *name) {
	if (!ConfMan.hasKey("metal_shader_path")) return Common::String();
	Common::Path root = ConfMan.getPath("metal_shader_path");
	if (root.empty()) return Common::String();
	for (const char *p = name; *p; ++p)
		if (*p == '/' || *p == '\\' || *p == ':') error("Metal: shader override requires a catalog basename");
	if (!strcmp(name, ".") || !strcmp(name, "..")) error("Metal: invalid shader override name");
	Common::FSNode node(root.join(engine).join(Common::String(name) + ".metal"));
	if (!node.exists()) return Common::String();
	Common::SeekableReadStream *stream = node.createReadStream();
	if (!stream) error("Metal: cannot read native shader override %s/%s", engine, name);
	int64 length = stream->size();
	if (length <= 0 || length > 4 * 1024 * 1024) { delete stream; error("Metal: invalid native shader override size"); }
	Common::Array<char> data; data.resize(length + 1);
	bool valid = stream->read(data.data(), length) == uint64(length) && !memchr(data.data(), 0, length);
	delete stream;
	if (!valid) error("Metal: invalid native shader override contents");
	data[length] = 0;
	return Common::String(data.data());
}

} }

#endif
