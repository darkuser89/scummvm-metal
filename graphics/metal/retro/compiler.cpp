/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphics/metal/retro/compiler.h"
#include "common/stream.h"
#include "common/textconsole.h"
#include <shaderc/shaderc.h>
#include <spirv_cross/spirv_cross_c.h>
#include <cmath>

namespace Graphics { namespace Metal { namespace Retro {

void readParameterDefaults(const Common::String &source, UniformsMap &parameters) {
	const char *line = source.c_str();
	while ((line = strstr(line, "#pragma parameter"))) {
		char name[128], label[256]; float initial, minimum, maximum, step;
		if (sscanf(line, "#pragma parameter %127s \"%255[^\"]\" %f %f %f %f", name, label, &initial, &minimum, &maximum, &step) >= 5)
			parameters[name] = initial;
		++line;
	}
}

Common::SeekableReadStream *SourceReader::open(const Common::Path &path) const {
	if (_container) {
		Common::SeekableReadStream *stream = _container->createReadStreamForMember(path);
		return stream ? stream : _archives.createReadStreamForMemberNext(path, _container);
	}
	return Common::FSNode(path).createReadStream();
}

bool SourceReader::read(const Common::Path &path, Common::String &text) const {
	Common::SeekableReadStream *stream = open(path);
	if (!stream) return false;
	const int64 size = stream->size();
	if (size < 0 || size > 16 * 1024 * 1024) { delete stream; return false; }
	Common::Array<char> data;
	data.resize(size + 1);
	bool success = stream->read(data.begin(), size) == uint32(size) && !stream->err();
	data[size] = 0;
	delete stream;
	if (success) {
		text = Common::String(data.begin(), size);
		if (_parameters) readParameterDefaults(text, *_parameters);
	}
	return success;
}

void CompiledShader::setUniform(const Common::String &name, const float *values, uint count) {
	setUniformValues(name, values, nullptr, count);
}

void CompiledShader::setUniformValues(const Common::String &name, const float *values, const uint32 *integers, uint count) {
	for (const auto &uniform : uniforms) {
		Common::String bindingName = uniform.name;
		if (bindingName.hasPrefix("_scummvmValue_")) bindingName.erase(0, 14);
		if ((bindingName != name && bindingName != name + "[0]") || count != uniform.components * uniform.columns) continue;
		Common::Array<byte> &data = uniform.fragment ? fragmentData : vertexData;
		for (uint column = 0; column < uniform.columns; ++column) {
			for (uint row = 0; row < uniform.components; ++row) {
				uint offset = uniform.offset + (uniform.columns > 1 ? column * uniform.matrixStride : 0) + row * 4;
				uint index = column * uniform.components + row;
				if (uniform.rowMajor && uniform.columns > 1) {
					offset = uniform.offset + row * uniform.matrixStride + column * 4;
				}
				if (offset + 4 > data.size()) continue;
				if (uniform.type == ShaderUniform::kFloat) {
					float value = integers ? float(integers[index]) : values[index];
					memcpy(data.begin() + offset, &value, 4);
				} else if (uniform.type == ShaderUniform::kBool) {
					uint32 value = integers ? integers[index] != 0 : values[index] != 0;
					memcpy(data.begin() + offset, &value, 4);
				} else if (integers) {
					// Preserve all 32 bits, as glUniform1i does for FrameCount.
					memcpy(data.begin() + offset, integers + index, 4);
				} else if (uniform.type == ShaderUniform::kInt) {
					int32 value = std::isfinite(values[index]) ? int32(CLIP<double>(values[index], -2147483648., 2147483647.)) : 0;
					memcpy(data.begin() + offset, &value, 4);
				} else {
					uint32 value = std::isfinite(values[index]) ? uint32(CLIP<double>(values[index], 0., 4294967295.)) : 0;
					memcpy(data.begin() + offset, &value, 4);
				}
			}
		}
		if (uniform.name.hasPrefix("_scummvmValue_")) {
			Common::String flagName = "_scummvmHas_", indices;
			for (uint pos = 0; pos < bindingName.size(); ++pos) {
				if (bindingName[pos] == '[') {
					do { indices += bindingName[pos]; } while (bindingName[pos] != ']' && ++pos < bindingName.size());
				} else flagName += bindingName[pos] == '.' ? '_' : bindingName[pos];
			}
			flagName += indices;
			for (const auto &flag : uniforms) {
				if (flag.name != flagName || flag.fragment != uniform.fragment || flag.offset + 4 > data.size()) continue;
				uint32 enabled = 1; memcpy(data.begin() + flag.offset, &enabled, 4);
			}
		}
	}
}

bool CompiledShader::hasUniform(const Common::String &name) const {
	for (const auto &uniform : uniforms)
		if (uniform.name == name || uniform.name == "_scummvmValue_" + name) return true;
	return false;
}

namespace {

struct Token {
	Common::String text;
	uint start, end;
};

static bool identifier(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9');
}

// Keep source spans rather than interpreting expressions. shaderc handles the
// actual GLSL grammar; this pass only relocates legacy global declarations.
static Common::Array<Token> tokenize(const Common::String &source) {
	Common::Array<Token> tokens;
	for (uint pos = 0; pos < source.size();) {
		char c = source[pos];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++pos; continue; }
		if (c == '#') { while (pos < source.size() && source[pos] != '\n') ++pos; continue; }
		if (c == '/' && pos + 1 < source.size() && source[pos + 1] == '/') {
			while (pos < source.size() && source[pos] != '\n') ++pos;
			continue;
		}
		if (c == '/' && pos + 1 < source.size() && source[pos + 1] == '*') {
			pos += 2;
			while (pos + 1 < source.size() && !(source[pos] == '*' && source[pos + 1] == '/')) ++pos;
			pos = MIN<uint>(source.size(), pos + 2);
			continue;
		}
		uint start = pos++;
		if (identifier(c)) while (pos < source.size() && identifier(source[pos])) ++pos;
		Token token = {Common::String(source.c_str() + start, pos - start), start, pos};
		tokens.push_back(token);
	}
	return tokens;
}

struct Edit {
	uint start, end;
	Common::String replacement;
};

struct StructMember {
	Common::String type, name, arrays;
};
typedef Common::HashMap<Common::String, Common::Array<StructMember> > Structures;

static Structures readStructures(const Common::String &source, const Common::Array<Token> &tokens) {
	Structures structures;
	uint scopeDepth = 0;
	for (uint i = 0; i + 2 < tokens.size(); ++i) {
		if (tokens[i].text == "{") { ++scopeDepth; continue; }
		if (tokens[i].text == "}") { if (scopeDepth) --scopeDepth; continue; }
		if (scopeDepth) continue;
		if (tokens[i].text != "struct" || tokens[i + 2].text != "{") continue;
		Common::Array<StructMember> members;
		uint begin = i + 3;
		while (begin < tokens.size() && tokens[begin].text != "}") {
			uint end = begin;
			while (end < tokens.size() && tokens[end].text != ";" && tokens[end].text != "}") ++end;
			if (end == tokens.size() || tokens[end].text != ";") break;
			uint type = begin;
			while (type < end && (tokens[type].text == "lowp" || tokens[type].text == "mediump" || tokens[type].text == "highp")) ++type;
			uint variable = type + 1;
			for (uint pos = variable; pos <= end; ++pos) {
				if (tokens[pos].text != "," && pos != end) continue;
				StructMember member;
				member.type = tokens[type].text; member.name = tokens[variable].text;
				member.arrays = Common::String(source.c_str() + tokens[variable].end, tokens[pos - 1].end - tokens[variable].end);
				members.push_back(member); variable = pos + 1;
			}
			begin = end + 1;
		}
		structures[tokens[i + 1].text] = members;
		i = begin;
	}
	return structures;
}

static bool initializeDefault(const Structures &structures, const Common::String &type,
		const Common::String &expression, const Common::String &flagName,
		const Common::String &arrays, const Common::String &parentArrays, const Common::String &parentIndices,
		Common::String &members, Common::String &statements, uint depth = 0) {
	if (depth > 16) return false;
	Common::String reference = expression, indices = parentIndices, loops;
	uint dimensions = 0;
	for (uint pos = 0; pos < arrays.size(); ++pos) {
		if (arrays[pos] != '[') continue;
		Common::String index = Common::String::format("_scummvmIndex%u_%u", depth, dimensions++);
		loops += "for(int " + index + "=0; " + index + "<" + reference + ".length(); ++" + index + ") {\n";
		reference += '[' + index + ']'; indices += '[' + index + ']';
	}
	Common::String body;
	auto structure = structures.find(type);
	if (structure != structures.end()) {
		for (const auto &field : structure->_value) {
			if (!initializeDefault(structures, field.type, reference + '.' + field.name, flagName + '_' + field.name,
				field.arrays, parentArrays + arrays, indices, members, body, depth + 1)) return false;
		}
	} else {
		members += "uint " + flagName + parentArrays + arrays + ";\n";
		body = "if(" + flagName + indices + "!=0u) " + reference + "=_scummvmValue_" + reference + ";\n";
	}
	statements += loops + body;
	for (uint dimension = 0; dimension < dimensions; ++dimension) statements += "}\n";
	return true;
}

static Common::String applyEdits(const Common::String &source, const Common::Array<Edit> &edits) {
	Common::String result;
	uint cursor = 0;
	for (const auto &edit : edits) {
		result += Common::String(source.c_str() + cursor, edit.start - cursor);
		result += edit.replacement;
		cursor = edit.end;
	}
	result += source.c_str() + cursor;
	return result;
}

static uint nameIndex(Common::StringArray &names, const Common::String &name) {
	for (uint i = 0; i < names.size(); ++i) if (names[i] == name) return i;
	names.push_back(name);
	return names.size() - 1;
}

static bool precisionQualifier(const Common::String &name) {
	return name == "highp" || name == "mediump" || name == "lowp";
}

static uint varyingLocation(Common::StringArray &slots, const Common::String &name, uint count) {
	for (uint i = 0; i < slots.size(); ++i) if (slots[i] == name) return i;
	uint location = slots.size();
	slots.push_back(name);
	for (uint i = 1; i < count; ++i) slots.push_back("");
	return location;
}

static Common::String cleanVersion(const Common::String &source) {
	Common::String result;
	for (uint begin = 0; begin < source.size();) {
		uint end = begin;
		while (end < source.size() && source[end] != '\n') ++end;
		Common::String line(source.c_str() + begin, end - begin);
		Common::String trimmed = line; trimmed.trim();
		if (!trimmed.hasPrefix("#version") && !trimmed.hasPrefix("#pragma parameter")) result += line;
		result += '\n';
		begin = end + 1;
	}
	return result;
}

static Common::String prependDeclarations(const Common::String &source, const Common::String &declarations) {
	Common::String cleaned = cleanVersion(source);
	Common::Array<Token> tokens = tokenize(cleaned);
	uint position = tokens.empty() ? cleaned.size() : tokens[0].start;
	// Extension directives must precede declarations, including generated ones.
	return "#version 450\n" + Common::String(cleaned.c_str(), position) + declarations + (cleaned.c_str() + position);
}

static Common::String hoistUniformDependencies(const Common::String &source) {
	const Common::Array<Token> tokens = tokenize(source);
	Common::Array<Edit> edits;
	Common::String declarations;
	uint blockDepth = 0, parenthesisDepth = 0;
	for (uint i = 0; i < tokens.size(); ++i) {
		if (tokens[i].text == "{") { ++blockDepth; continue; }
		if (tokens[i].text == "}") { if (blockDepth) --blockDepth; continue; }
		if (blockDepth) continue;
		if (tokens[i].text == "(") { ++parenthesisDepth; continue; }
		if (tokens[i].text == ")") { if (parenthesisDepth) --parenthesisDepth; continue; }
		if (parenthesisDepth) continue;
		bool structure = tokens[i].text == "struct" && i + 2 < tokens.size() && tokens[i + 2].text == "{";
		if (!structure && tokens[i].text != "const") continue;
		uint end = i + 1, closing = 0, depth = 0;
		for (; end < tokens.size(); ++end) {
			if (tokens[end].text == "{") ++depth;
			if (tokens[end].text == "}" && depth && !--depth) closing = end;
			if (tokens[end].text == ";" && !depth) break;
		}
		if (end == tokens.size() || (structure && !closing)) continue;
		Edit edit = {tokens[i].start, tokens[end].end, ""};
		if (!structure) {
			uint begin = i;
			while (begin && precisionQualifier(tokens[begin - 1].text)) --begin;
			edit.start = tokens[begin].start;
		}
		if (structure) {
			declarations += Common::String(source.c_str() + edit.start, tokens[closing].end - edit.start) + ";\n";
			// Preserve instances at their original declaration point. Only the
			// named type must precede the combined uniform block.
			if (closing + 1 < end)
				edit.replacement = tokens[i + 1].text + ' ' + Common::String(source.c_str() + tokens[closing + 1].start,
					tokens[end].end - tokens[closing + 1].start);
		} else {
			declarations += Common::String(source.c_str() + edit.start, edit.end - edit.start) + '\n';
		}
		edits.push_back(edit); i = end;
	}
	// A later uniform can use a struct or array-size constant declared after
	// an earlier uniform. Relocating the block must retain those dependencies.
	return declarations.empty() ? source : prependDeclarations(applyEdits(source, edits), declarations);
}

static Common::String deferGlobalInitializers(const Common::String &source,
		const Common::StringArray &initializedUniforms, Common::String &initializers) {
	const Common::Array<Token> tokens = tokenize(source);
	const Structures structures = readStructures(source, tokens);
	Common::Array<Edit> edits;
	uint scope = 0;
	for (uint i = 0; i + 2 < tokens.size(); ++i) {
		if (tokens[i].text == "{") { ++scope; continue; }
		if (tokens[i].text == "}") { if (scope) --scope; continue; }
		if (scope || (i && tokens[i - 1].text != ";" && tokens[i - 1].text != "}")) continue;
		uint type = i;
		while (type < tokens.size() && precisionQualifier(tokens[type].text)) ++type;
		if (type + 2 >= tokens.size()) continue;
		const Common::String &name = tokens[type].text;
		bool numeric = name == "float" || name == "double" || name == "int" || name == "uint" || name == "bool" ||
			name.hasPrefix("vec") || name.hasPrefix("ivec") || name.hasPrefix("uvec") || name.hasPrefix("bvec") ||
			name.hasPrefix("dvec") || name.hasPrefix("mat") || name.hasPrefix("dmat");
		if ((!numeric && !structures.contains(name)) || tokens[type + 2].text == "(") continue;
		uint variable = type + 1, initializer = uint(-1), depth = 0;
		for (uint pos = variable; pos < tokens.size(); ++pos) {
			const Common::String &token = tokens[pos].text;
			if (token == "(" || token == "[" || token == "{") ++depth;
			else if (token == ")" || token == "]" || token == "}") { if (depth) --depth; }
			else if (token == "=" && !depth && initializer == uint(-1)) initializer = pos;
			if ((token == "," || token == ";") && !depth) {
				bool uniformDefault = false;
				for (const Common::String &uniform : initializedUniforms)
					if (uniform == tokens[variable].text) uniformDefault = true;
				if (initializer != uint(-1) && !uniformDefault) {
					initializers += tokens[variable].text + ' ' + Common::String(source.c_str() + tokens[initializer].start,
						tokens[pos].start - tokens[initializer].start) + ";\n";
					Edit edit = {tokens[initializer].start, tokens[pos].start, ""};
					edits.push_back(edit);
				}
				variable = pos + 1; initializer = uint(-1);
				if (token == ";") { i = pos; break; }
			}
		}
	}
	return applyEdits(source, edits);
}

static bool collectBooleanUniforms(const Structures &structures, const Common::String &type,
		const Common::String &name, Common::StringArray &booleans, uint depth = 0) {
	if (depth > 16) return false;
	auto structure = structures.find(type);
	if (structure != structures.end()) {
		for (const auto &member : structure->_value)
			if (!collectBooleanUniforms(structures, member.type, name + '.' + member.name, booleans, depth + 1)) return false;
	} else if (type == "bool" || type == "bvec2" || type == "bvec3" || type == "bvec4") {
		booleans.push_back(name);
	}
	return true;
}

static uint interfaceSlots(spvc_compiler compiler, spvc_type type, uint depth = 0) {
	if (depth > 16) return 0;
	uint slots = spvc_type_get_columns(type);
	if (spvc_type_get_basetype(type) == SPVC_BASETYPE_STRUCT) {
		slots = 0;
		for (uint member = 0; member < spvc_type_get_num_member_types(type); ++member) {
			uint memberSlots = interfaceSlots(compiler, spvc_compiler_get_type_handle(compiler,
				spvc_type_get_member_type(type, member)), depth + 1);
			if (!memberSlots || memberSlots > 4096 - slots) return 0;
			slots += memberSlots;
		}
	}
	for (uint dimension = 0; dimension < spvc_type_get_num_array_dimensions(type); ++dimension) {
		uint count = spvc_type_get_array_dimension(type, dimension);
		if (!spvc_type_array_dimension_is_literal(type, dimension) || !count || count > 4096 || slots > 4096 / count) return 0;
		slots *= count;
	}
	return slots;
}

static void clearMemberLocations(spvc_compiler compiler, spvc_type type, uint depth = 0) {
	if (depth > 16 || spvc_type_get_basetype(type) != SPVC_BASETYPE_STRUCT) return;
	spvc_type_id base = spvc_type_get_base_type_id(type);
	for (uint member = 0; member < spvc_type_get_num_member_types(type); ++member) {
		spvc_compiler_unset_member_decoration(compiler, base, member, SpvDecorationLocation);
		clearMemberLocations(compiler, spvc_compiler_get_type_handle(compiler, spvc_type_get_member_type(type, member)), depth + 1);
	}
}

struct Include {
	shaderc_include_result result = {};
	Common::String name, content;
};

static shaderc_include_result *resolveInclude(void *user, const char *request, int type,
		const char *requesting, size_t depth) {
	const SourceReader &reader = *static_cast<const SourceReader *>(user);
	Include *include = new Include();
	Common::Path path = Common::Path(requesting).getParent().join(Common::Path(request)).normalize();
	bool success = depth <= 32 && reader.read(path, include->content);
	if (!success && type == shaderc_include_type_standard) {
		path = Common::Path(request).normalize();
		success = depth <= 32 && reader.read(path, include->content);
	}
	if (success) {
		include->name = path.toString('/');
		include->content = cleanVersion(include->content);
	} else {
		include->content = Common::String::format("Could not read shader include '%s'", request);
	}
	include->result.source_name = include->name.c_str();
	include->result.source_name_length = include->name.size();
	include->result.content = include->content.c_str();
	include->result.content_length = include->content.size();
	include->result.user_data = include;
	return &include->result;
}

static void releaseInclude(void *, shaderc_include_result *result) {
	delete static_cast<Include *>(result->user_data);
}

static bool preprocess(shaderc_compiler_t compiler, shaderc_compile_options_t options,
		const Common::Path &path, const Common::String &source, const Common::String &aliases,
		bool fragment, Common::String &output, Common::String &error) {
	Common::String input = "#version 450\n#define PARAMETER_UNIFORM\n";
	input += fragment ? "#define FRAGMENT\n#define varying in\n" : "#define VERTEX\n#define attribute in\n#define varying out\n";
	input += "#define texture2D texture\n#define textureCube texture\n#define texture2DLod textureLod\n#define texture2DProj textureProj\n";
	input += "#define gl_VertexID gl_VertexIndex\n#define gl_InstanceID gl_InstanceIndex\n";
	input += aliases;
	input += cleanVersion(source);
	const Common::String fileName = path.toString('/');
	shaderc_compilation_result_t result = shaderc_compile_into_preprocessed_text(compiler,
		input.c_str(), input.size(), fragment ? shaderc_fragment_shader : shaderc_vertex_shader,
		fileName.c_str(), "main", options);
	bool success = result && shaderc_result_get_compilation_status(result) == shaderc_compilation_status_success;
	if (success) output = Common::String(shaderc_result_get_bytes(result), shaderc_result_get_length(result));
	else error = result ? shaderc_result_get_error_message(result) : "shaderc preprocessing allocation failed";
	shaderc_result_release(result);
	return success;
}

static bool rewrite(const Common::String &input, bool fragment, Common::StringArray &samplers, Common::StringArray &booleans,
		Common::String &output, Common::String &error) {
	const Common::String source = hoistUniformDependencies(input);
	const Common::Array<Token> tokens = tokenize(source);
	const Structures structures = readStructures(source, tokens);
	Common::Array<Edit> edits;
	Common::String members;
	Common::String initializeUniforms;
	Common::StringArray initializedUniformNames;
	uint firstUniform = uint(-1), blockDepth = 0, parenthesisDepth = 0, declaration = 0;
	bool legacyOutput = false;
	for (uint i = 0; i < tokens.size(); ++i) {
		const Token &token = tokens[i];
		if (token.text == "gl_FragColor") legacyOutput = true;
		if (token.text == "{") { ++blockDepth; continue; }
		if (token.text == "}") { if (blockDepth) --blockDepth; if (!blockDepth) declaration = i + 1; continue; }
		if (blockDepth) continue;
		if (token.text == "(") { ++parenthesisDepth; continue; }
		if (token.text == ")") { if (parenthesisDepth) --parenthesisDepth; continue; }
		if (parenthesisDepth) continue;
		if (token.text == ";") { declaration = i + 1; continue; }
		if (token.text != "uniform" && token.text != "in" && token.text != "out" && token.text != "precision") continue;
		uint end = i + 1;
		while (end < tokens.size() && tokens[end].text != ";" && tokens[end].text != "{") ++end;
		if (end < tokens.size() && tokens[end].text == "{" && (token.text == "in" || token.text == "out")) {
			uint depth = 1; ++end;
			while (end < tokens.size() && depth) {
				if (tokens[end].text == "{") ++depth;
				if (tokens[end].text == "}") --depth;
				++end;
			}
			while (end < tokens.size() && tokens[end].text != ";") ++end;
			if (end == tokens.size()) { error = "Incomplete GLSL interface block"; return false; }
			i = end; declaration = end + 1; continue;
		}
		if (end == tokens.size() || tokens[end].text != ";") {
			error = "Preset uniform blocks do not expose the classic named-uniform interface"; return false;
		}
		uint type = i + 1;
		while (type < end && precisionQualifier(tokens[type].text)) ++type;
		Edit edit = {tokens[declaration].start, tokens[end].end, ""};
		if (token.text == "precision") {
			// Metal receives typed SPIR-V, without GLSL ES precision statements.
		} else if (token.text == "uniform") {
			if (type + 1 >= end) { error = "Incomplete uniform declaration"; return false; }
			if (tokens[type].text.hasPrefix("sampler")) {
				uint variable = type + 1, bracketDepth = 0;
				for (uint pos = variable; pos <= end; ++pos) {
					if (tokens[pos].text == "[") ++bracketDepth;
					if (tokens[pos].text == "]" && bracketDepth) --bracketDepth;
					if ((tokens[pos].text == "," && !bracketDepth) || pos == end) {
						if (variable >= pos || (!identifier(tokens[variable].text[0]) ||
						    (tokens[variable].text[0] >= '0' && tokens[variable].text[0] <= '9'))) {
							error = "Incomplete sampler declarator"; return false;
						}
						uint slot = nameIndex(samplers, tokens[variable].text);
						Common::String variableText(source.c_str() + tokens[variable].start, tokens[pos - 1].end - tokens[variable].start);
						edit.replacement += Common::String::format("layout(set=0,binding=%u) uniform %s %s;\n",
							slot + 1, tokens[type].text.c_str(), variableText.c_str());
						variable = pos + 1;
					}
				}
			} else {
				if (firstUniform == uint(-1)) firstUniform = edits.size();
				uint variable = type + 1, brackets = 0, parentheses = 0;
				for (uint pos = variable; pos <= end; ++pos) {
					if (tokens[pos].text == "[") ++brackets;
					if (tokens[pos].text == "]" && brackets) --brackets;
					if (tokens[pos].text == "(") ++parentheses;
					if (tokens[pos].text == ")" && parentheses) --parentheses;
					if ((tokens[pos].text == "," && !brackets && !parentheses) || pos == end) {
						if (variable >= pos || (!identifier(tokens[variable].text[0]) ||
						    (tokens[variable].text[0] >= '0' && tokens[variable].text[0] <= '9'))) {
							error = "Incomplete uniform declarator"; return false;
						}
						uint initializer = variable + 1;
						while (initializer < pos && tokens[initializer].text != "=") ++initializer;
						Common::String declarationText(source.c_str() + tokens[variable].start,
							tokens[initializer - 1].end - tokens[variable].start);
						Common::String storageName = (initializer == pos ? "" : "_scummvmValue_") + tokens[variable].text;
						if (!collectBooleanUniforms(structures, tokens[type].text, storageName, booleans)) {
							error = "Boolean uniform structure exceeds nesting limit"; return false;
						}
						if (initializer == pos) {
							members += tokens[type].text + ' ' + declarationText + ";\n";
						} else {
							Common::String name = tokens[variable].text;
							initializedUniformNames.push_back(name);
							Common::String suffix(source.c_str() + tokens[variable].end,
								tokens[initializer - 1].end - tokens[variable].end);
							members += tokens[type].text + " _scummvmValue_" + name + suffix + ";\n";
							edit.replacement += tokens[type].text + ' ' + Common::String(source.c_str() + tokens[variable].start,
								tokens[pos - 1].end - tokens[variable].start) + ";\n";
							if (!initializeDefault(structures, tokens[type].text, name, "_scummvmHas_" + name,
								suffix, "", "", members, initializeUniforms)) { error = "Uniform initializer structure exceeds nesting limit"; return false; }
						}
						variable = pos + 1;
					}
				}
			}
		} else {
			// Interface expressions remain GLSL. Matching Metal varying slots
			// are assigned using the reflected types after shaderc resolves them.
			edit.replacement = Common::String(source.c_str() + edit.start, edit.end - edit.start);
		}
		edits.push_back(edit);
		i = end;
		declaration = end + 1;
	}
	Common::String block;
	if (!members.empty()) block = "layout(std140,set=0,binding=0) uniform ScummVMParameters {\n" + members + "};\n";
	if (firstUniform != uint(-1)) edits[firstUniform].replacement = block + edits[firstUniform].replacement;
	output = applyEdits(source, edits);
	if (firstUniform == uint(-1) && !block.empty()) output = prependDeclarations(output, block);
	if (fragment && legacyOutput) {
		output = prependDeclarations(output, "layout(location=0) out vec4 _scummvmColor;\n#define gl_FragColor _scummvmColor\n");
	}
	if (!initializeUniforms.empty()) {
		// Uniform defaults are private GLSL variables. Apply their overrides
		// before evaluating globals that depend on them, in declaration order.
		Common::String initializeGlobals;
		output = deferGlobalInitializers(output, initializedUniformNames, initializeGlobals);
		output = prependDeclarations(output, "#define main _scummvmOriginalMain\n");
		output += "\n#undef main\nvoid main() {\n" + initializeUniforms + initializeGlobals + "_scummvmOriginalMain();\n}\n";
	}
	return true;
}

static bool reflectMembers(spvc_compiler compiler, spvc_type structure, const Common::String &prefix,
		uint baseOffset, bool fragment, const Common::StringArray &booleans, CompiledShader &shader, Common::String &error, uint depth = 0) {
	if (depth > 16) { error = "Uniform structure nesting exceeds reflection limit"; return false; }
	const spvc_type_id structureId = spvc_type_get_base_type_id(structure);
	for (uint i = 0; i < spvc_type_get_num_member_types(structure); ++i) {
		unsigned offset = 0, arrayStride = 0, matrixStride = 0;
		if (spvc_compiler_type_struct_member_offset(compiler, structure, i, &offset) != SPVC_SUCCESS) return false;
		spvc_type type = spvc_compiler_get_type_handle(compiler, spvc_type_get_member_type(structure, i));
		Common::String name = prefix + spvc_compiler_get_member_name(compiler, structureId, i);
		uint elements = 1, innerElements = 1;
		uint dimensions = spvc_type_get_num_array_dimensions(type);
		Common::Array<uint> counts;
		if (dimensions) {
			for (uint dimension = 0; dimension < dimensions; ++dimension) {
				if (!spvc_type_array_dimension_is_literal(type, dimension)) { error = "Runtime uniform arrays cannot be bound to a preset"; return false; }
				uint count = spvc_type_get_array_dimension(type, dimension);
				if (!count || count > 4096 || elements > 4096 / count) return false;
				counts.push_back(count); elements *= count;
				if (dimension + 1 < dimensions) innerElements *= count;
			}
			if (spvc_compiler_type_struct_member_array_stride(compiler, structure, i, &arrayStride) != SPVC_SUCCESS || arrayStride % innerElements) return false;
			arrayStride /= innerElements;
		}
		uint columns = spvc_type_get_columns(type);
		if (columns > 1 && spvc_compiler_type_struct_member_matrix_stride(compiler, structure, i, &matrixStride) != SPVC_SUCCESS) return false;
		for (uint element = 0; element < elements; ++element) {
			Common::String elementName = name;
			if (dimensions) {
				Common::Array<uint> indices; indices.resize(dimensions);
				uint remainder = element;
				for (uint dimension = 0; dimension < dimensions; ++dimension) {
					indices[dimension] = remainder % counts[dimension]; remainder /= counts[dimension];
				}
				for (uint dimension = dimensions; dimension; --dimension)
					elementName += Common::String::format("[%u]", indices[dimension - 1]);
			}
			uint elementOffset = baseOffset + offset + element * arrayStride;
			if (spvc_type_get_basetype(type) == SPVC_BASETYPE_STRUCT) {
				if (!reflectMembers(compiler, type, elementName + '.', elementOffset, fragment, booleans, shader, error, depth + 1)) return false;
				continue;
			}
			ShaderUniform uniform;
			uniform.name = elementName; uniform.offset = elementOffset; uniform.fragment = fragment;
			uniform.components = spvc_type_get_vector_size(type); uniform.columns = columns;
			uniform.matrixStride = matrixStride;
			uniform.rowMajor = spvc_compiler_has_member_decoration(compiler, structureId, i, SpvDecorationRowMajor);
			switch (spvc_type_get_basetype(type)) {
			case SPVC_BASETYPE_FP32: uniform.type = ShaderUniform::kFloat; break;
			case SPVC_BASETYPE_INT32: uniform.type = ShaderUniform::kInt; break;
			case SPVC_BASETYPE_UINT32: uniform.type = ShaderUniform::kUInt; break;
			case SPVC_BASETYPE_BOOLEAN: uniform.type = ShaderUniform::kBool; break;
			default: error = "Preset requires a uniform type outside the classic 32-bit interface"; return false;
			}
			// Vulkan lowers booleans stored in a block to uint32. Preserve the
			// source type so negative parameter values still mean true.
			Common::String leaf;
			for (uint pos = 0; pos < elementName.size(); ++pos) {
				if (elementName[pos] == '[') {
					while (pos < elementName.size() && elementName[pos] != ']') ++pos;
				} else leaf += elementName[pos];
			}
			for (const auto &boolean : booleans)
				if (boolean == leaf) uniform.type = ShaderUniform::kBool;
			shader.uniforms.push_back(uniform);
		}
	}
	return true;
}

static bool compileStage(shaderc_compiler_t compiler, shaderc_compile_options_t options,
		const Common::Path &path, const Common::String &source, bool fragment,
		const Common::StringArray &declaredSamplers, const Common::StringArray &booleans, Common::StringArray &varyings, CompiledShader &shader,
		Common::String &msl, Common::String &entry, Common::String &error) {
	const Common::String fileName = path.toString('/');
	shaderc_compilation_result_t result = shaderc_compile_into_spv(compiler, source.c_str(), source.size(),
		fragment ? shaderc_fragment_shader : shaderc_vertex_shader, fileName.c_str(), "main", options);
	if (!result || shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
		error = result ? shaderc_result_get_error_message(result) : "shaderc compilation allocation failed";
		shaderc_result_release(result); return false;
	}
	spvc_context context = nullptr;
	spvc_parsed_ir ir = nullptr;
	spvc_compiler cross = nullptr;
	spvc_resources resources = nullptr;
	spvc_compiler_options crossOptions = nullptr;
	bool success = spvc_context_create(&context) == SPVC_SUCCESS;
	if (success) success = spvc_context_parse_spirv(context,
		reinterpret_cast<const SpvId *>(shaderc_result_get_bytes(result)), shaderc_result_get_length(result) / 4, &ir) == SPVC_SUCCESS;
	if (success) success = spvc_context_create_compiler(context, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &cross) == SPVC_SUCCESS;
	shaderc_result_release(result);
	if (success) success = spvc_compiler_create_compiler_options(cross, &crossOptions) == SPVC_SUCCESS;
	if (success) success = spvc_compiler_options_set_uint(crossOptions, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(3, 0, 0)) == SPVC_SUCCESS &&
		spvc_compiler_options_set_uint(crossOptions, SPVC_COMPILER_OPTION_MSL_PLATFORM, SPVC_MSL_PLATFORM_MACOS) == SPVC_SUCCESS &&
		spvc_compiler_options_set_bool(crossOptions, SPVC_COMPILER_OPTION_FIXUP_DEPTH_CONVENTION, SPVC_TRUE) == SPVC_SUCCESS &&
		spvc_compiler_options_set_bool(crossOptions, SPVC_COMPILER_OPTION_FLIP_VERTEX_Y, SPVC_TRUE) == SPVC_SUCCESS &&
		spvc_compiler_install_compiler_options(cross, crossOptions) == SPVC_SUCCESS;
	spvc_set active = nullptr;
	if (success) success = spvc_compiler_get_active_interface_variables(cross, &active) == SPVC_SUCCESS &&
		spvc_compiler_set_enabled_interface_variables(cross, active) == SPVC_SUCCESS &&
		spvc_compiler_create_shader_resources_for_active_variables(cross, &resources, active) == SPVC_SUCCESS;
	const SpvExecutionModel model = fragment ? SpvExecutionModelFragment : SpvExecutionModelVertex;
	const spvc_reflected_resource *list = nullptr;
	size_t count = 0;
	if (success) success = spvc_resources_get_resource_list_for_type(resources,
		fragment ? SPVC_RESOURCE_TYPE_STAGE_INPUT : SPVC_RESOURCE_TYPE_STAGE_OUTPUT, &list, &count) == SPVC_SUCCESS;
	for (size_t i = 0; success && i < count; ++i) {
		spvc_type type = spvc_compiler_get_type_handle(cross, list[i].type_id);
		uint slots = interfaceSlots(cross, type);
		if (!slots) { error = "Could not resolve preset varying structure or array dimensions"; success = false; break; }
		Common::String name = list[i].name;
		if (spvc_type_get_basetype(type) == SPVC_BASETYPE_STRUCT) {
			spvc_type_id base = spvc_type_get_base_type_id(type);
			// Interface blocks match by their type name, even when the two
			// stages use different instance names. Allocate their full span,
			// including nested structs and arrays, at one base location.
			if (spvc_compiler_has_decoration(cross, base, SpvDecorationBlock)) name = spvc_compiler_get_name(cross, base);
			clearMemberLocations(cross, type);
		}
		spvc_compiler_set_decoration(cross, list[i].id, SpvDecorationLocation, varyingLocation(varyings, name, slots));
	}
	if (success) success = spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &count) == SPVC_SUCCESS;
	if (success && count > 1) { error = "Preset contains more than one uniform block per stage"; success = false; }
	for (size_t i = 0; success && i < count; ++i) {
		spvc_type type = spvc_compiler_get_type_handle(cross, list[i].base_type_id);
		size_t size = 0;
		success = spvc_compiler_get_declared_struct_size(cross, type, &size) == SPVC_SUCCESS && size <= 64 * 1024;
		if (success) {
			Common::Array<byte> &data = fragment ? shader.fragmentData : shader.vertexData;
			data.resize(MAX<size_t>(16, size)); memset(data.begin(), 0, data.size());
			success = reflectMembers(cross, type, "", 0, fragment, booleans, shader, error);
		}
		spvc_msl_resource_binding binding; spvc_msl_resource_binding_init(&binding);
		binding.stage = model; binding.desc_set = 0; binding.binding = 0; binding.msl_buffer = 1;
		if (success) success = spvc_compiler_msl_add_resource_binding(cross, &binding) == SPVC_SUCCESS;
	}
	if (success) success = spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &list, &count) == SPVC_SUCCESS;
	for (size_t i = 0; success && i < count; ++i) {
		uint slot = spvc_compiler_get_decoration(cross, list[i].id, SpvDecorationBinding);
		if (!slot || slot > declaredSamplers.size()) { error = "Invalid translated sampler binding"; success = false; break; }
		spvc_type type = spvc_compiler_get_type_handle(cross, list[i].type_id);
		if (spvc_type_get_image_dimension(type) != SpvDim2D || spvc_type_get_image_arrayed(type) || spvc_type_get_image_multisampled(type)) {
			error = "Classic presets require single-layer 2D texture samplers"; success = false; break;
		}
		uint elements = 1;
		for (uint dimension = 0; dimension < spvc_type_get_num_array_dimensions(type); ++dimension) {
			if (!spvc_type_array_dimension_is_literal(type, dimension)) { success = false; break; }
			uint arraySize = spvc_type_get_array_dimension(type, dimension);
			if (!arraySize || arraySize > 16 || elements > 16 / arraySize) { success = false; break; }
			elements *= arraySize;
		}
		uint nativeSlot = 0;
		for (uint element = 0; success && element < elements; ++element) {
			Common::String name = declaredSamplers[slot - 1];
			if (spvc_type_get_num_array_dimensions(type)) name += Common::String::format("[%u]", element);
			uint mappedSlot = nameIndex(shader.samplers, name);
			if (!element) nativeSlot = mappedSlot;
			if (mappedSlot >= 16 || mappedSlot != nativeSlot + element) success = false;
		}
		if (!success) { error = "Preset exceeds contiguous native sampler array bindings"; break; }
		spvc_msl_resource_binding binding; spvc_msl_resource_binding_init(&binding);
		binding.stage = model; binding.desc_set = 0; binding.binding = slot;
		binding.msl_texture = binding.msl_sampler = nativeSlot;
		success = spvc_compiler_msl_add_resource_binding(cross, &binding) == SPVC_SUCCESS;
	}
	if (success && !fragment) {
		success = spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_INPUT, &list, &count) == SPVC_SUCCESS;
		for (size_t i = 0; success && i < count; ++i) {
			spvc_type type = spvc_compiler_get_type_handle(cross, list[i].type_id);
			uint location = spvc_compiler_get_decoration(cross, list[i].id, SpvDecorationLocation);
			uint slots = spvc_type_get_columns(type), components = spvc_type_get_vector_size(type);
			for (uint dimension = 0; dimension < spvc_type_get_num_array_dimensions(type); ++dimension) {
				uint elements = spvc_type_get_array_dimension(type, dimension);
				if (!spvc_type_array_dimension_is_literal(type, dimension) || !elements || elements > 31 || slots > 31 / elements) {
					success = false; break;
				}
				slots *= elements;
			}
			if (!success || location >= 31 || slots > 31 - location || !components || components > 4 ||
			    spvc_type_get_basetype(type) != SPVC_BASETYPE_FP32) {
				error = "Preset vertex attributes exceed the classic float-vector interface"; success = false; break;
			}
			for (uint slot = 0; slot < slots; ++slot) {
				ShaderAttribute attribute;
				attribute.name = list[i].name;
				attribute.layout.index = location + slot;
				attribute.layout.offset = shader.vertexStride;
				attribute.layout.components = components;
				// GL binds only the first attribute location by name. Remaining
				// matrix columns and array elements retain generic (0,0,0,1).
				attribute.generic = slot != 0;
				shader.vertexStride += components * 4;
				shader.attributes.push_back(attribute);
			}
		}
	}
	const char *name = fragment ? "scummvmPresetFragment" : "scummvmPresetVertex";
	if (success) success = spvc_compiler_rename_entry_point(cross, "main", name, model) == SPVC_SUCCESS;
	const char *translated = nullptr;
	if (success) success = spvc_compiler_compile(cross, &translated) == SPVC_SUCCESS;
	if (success) {
		msl = translated;
		entry = spvc_compiler_get_cleansed_entry_point_name(cross, name, model);
	} else if (error.empty()) {
		error = context ? spvc_context_get_last_error_string(context) : "SPIRV-Cross context allocation failed";
	}
	spvc_context_destroy(context);
	return success;
}

} // anonymous namespace

CompiledShader *ShaderCompiler::compile(const Common::Path &path, const Common::String &source, const Common::String &aliases) {
	_error.clear();
	CompiledShader *shader = new CompiledShader();
	shaderc_compiler_t compiler = shaderc_compiler_initialize();
	shaderc_compile_options_t options = shaderc_compile_options_initialize();
	if (!compiler || !options) {
		shaderc_compiler_release(compiler); shaderc_compile_options_release(options);
		delete shader; _error = "Could not initialize the native preset translator"; return nullptr;
	}
	shaderc_compile_options_set_source_language(options, shaderc_source_language_glsl);
	shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
	shaderc_compile_options_set_target_spirv(options, shaderc_spirv_version_1_3);
	shaderc_compile_options_set_auto_map_locations(options, true);
	shaderc_compile_options_set_include_callbacks(options, resolveInclude, releaseInclude, const_cast<SourceReader *>(&_reader));
	Common::String vertex, fragment, modernVertex, modernFragment, vertexMSL, fragmentMSL, vertexEntry, fragmentEntry;
	Common::StringArray varyings, declaredSamplers, vertexBooleans, fragmentBooleans;
	bool success = preprocess(compiler, options, path, source, aliases, false, vertex, _error) &&
		preprocess(compiler, options, path, source, aliases, true, fragment, _error) &&
		rewrite(vertex, false, declaredSamplers, vertexBooleans, modernVertex, _error) &&
		rewrite(fragment, true, declaredSamplers, fragmentBooleans, modernFragment, _error) &&
		compileStage(compiler, options, path, modernVertex, false, declaredSamplers, vertexBooleans, varyings, *shader, vertexMSL, vertexEntry, _error) &&
		compileStage(compiler, options, path, modernFragment, true, declaredSamplers, fragmentBooleans, varyings, *shader, fragmentMSL, fragmentEntry, _error);
	shaderc_compile_options_release(options); shaderc_compiler_release(compiler);
	if (success) {
		if (!shader->vertexStride) shader->vertexStride = 4;
		Common::Array<VertexAttribute> layout;
		for (const auto &attribute : shader->attributes) layout.push_back(attribute.layout);
		shader->program = _renderer.createProgramStages(vertexMSL.c_str(), vertexEntry.c_str(), fragmentMSL.c_str(), fragmentEntry.c_str(),
			layout.empty() ? nullptr : layout.begin(), layout.size(), shader->vertexStride);
		success = shader->program != nullptr;
		if (!success) _error = "Metal rejected the translated preset program";
	}
	if (!success) { delete shader; return nullptr; }
	return shader;
}

} } } // namespace Graphics::Metal::Retro
