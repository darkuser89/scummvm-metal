/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_GEOMETRY_H
#define GRAPHICS_METAL_GEOMETRY_H

#include "graphics/metal/renderer.h"
#include "common/hashmap.h"
#include "common/textconsole.h"

namespace Graphics { namespace Metal {

// Small immediate-mode engines have no mesh handles or mutation notifications.
// Compare their actual bytes, never their source addresses. Hash collisions only
// evict an entry; they cannot select different geometry. Both CPU and GPU copies
// have a fixed bound and entries are released safely through submission retention.
class GeometryCache : public Common::NonCopyable {
public:
	explicit GeometryCache(Renderer *renderer) : _renderer(renderer) {}
	~GeometryCache() { for (auto &entry : _entries) delete entry._value; }
	void draw(const Vertex *vertices, uint count, Texture *texture, const DrawState &state, const Uniforms &uniforms,
	          const uint32 *indices = nullptr, uint indexCount = 0) {
		if (!vertices || !count || (indexCount && !indices)) return;
		uint64 vertexBytes = uint64(count) * sizeof(Vertex), indexBytes = uint64(indexCount) * sizeof(uint32);
		uint64 bytes = vertexBytes + indexBytes;
		if (bytes > kLimit) {
			if (indexCount) _renderer->drawIndexed(vertices, count, indices, indexCount, texture, state, uniforms);
			else _renderer->draw(vertices, count, texture, state, uniforms);
			return;
		}
		uint64 hash = 14695981039346656037ULL;
		for (uint i = 0; i < vertexBytes; ++i) hash = (hash ^ ((const byte *)vertices)[i]) * 1099511628211ULL;
		for (uint i = 0; i < indexBytes; ++i) hash = (hash ^ ((const byte *)indices)[i]) * 1099511628211ULL;
		hash ^= vertexBytes;
		Entry *entry = _entries.contains(hash) ? _entries[hash] : nullptr;
		if (entry && (entry->count != count || entry->indexCount != indexCount ||
		    memcmp(entry->data.data(), vertices, vertexBytes) ||
		    (indexBytes && memcmp(entry->data.data() + vertexBytes, indices, indexBytes)))) {
			evictEntry(entry); entry = nullptr;
		}
		if (!entry) {
			while (!_entries.empty() && (_used + bytes > kLimit || _entries.size() >= 2048)) {
				evictEntry(_oldest);
			}
			for (uint i = 0; i < indexCount; ++i) if (indices[i] >= count) error("Metal: cached geometry index out of range");
			entry = new Entry; entry->key = hash; entry->count = count; entry->indexCount = indexCount;
			entry->data.resize(bytes); memcpy(entry->data.data(), vertices, vertexBytes);
			if (indexBytes) memcpy(entry->data.data() + vertexBytes, indices, indexBytes);
			entry->geometry.vertices = _renderer->createBuffer(vertices, vertexBytes);
			if (indexBytes) entry->geometry.indices = _renderer->createBuffer(indices, indexBytes);
			if (!entry->geometry.vertices || (indexBytes && !entry->geometry.indices)) { delete entry; error("Metal: could not cache geometry"); }
			_entries[hash] = entry; _used += bytes;
		}
		// Intrusive LRU links avoid scanning the hash table on every eviction.
		detachEntry(entry);
		entry->previous = _newest;
		if (_newest) _newest->next = entry; else _oldest = entry;
		_newest = entry;
		_renderer->drawBuffered(entry->geometry, count, indexCount, texture, state, uniforms);
	}
private:
	struct Entry {
		GeometryBinding geometry;
		Common::Array<byte> data;
		uint count = 0, indexCount = 0;
		uint64 key = 0;
		Entry *previous = nullptr, *next = nullptr;
		~Entry() { delete geometry.vertices; delete geometry.indices; }
	};
	enum { kLimit = 8 * 1024 * 1024 };
	Renderer *_renderer;
	Common::HashMap<uint64, Entry *> _entries;
	uint64 _used = 0;
	Entry *_oldest = nullptr, *_newest = nullptr;
	void detachEntry(Entry *entry) {
		if (entry->previous) entry->previous->next = entry->next;
		else if (_oldest == entry) _oldest = entry->next;
		if (entry->next) entry->next->previous = entry->previous;
		else if (_newest == entry) _newest = entry->previous;
		entry->previous = entry->next = nullptr;
	}
	void evictEntry(Entry *entry) {
		detachEntry(entry); _used -= entry->data.size();
		_entries.erase(entry->key); delete entry;
	}
};

} } // namespace Graphics::Metal

#endif
