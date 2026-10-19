/* ScummVM - Graphic Adventure Engine
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GRAPHICS_METAL_PIPELINE_CACHE_H
#define GRAPHICS_METAL_PIPELINE_CACHE_H

#import <CommonCrypto/CommonDigest.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <fcntl.h>
#include <unistd.h>

namespace Graphics { namespace Metal {

// Private Objective-C++ helper. Archives are disposable acceleration data, never
// an authority for shader interfaces. Metal validates every descriptor on load.
class PipelineArchiveCache {
public:
	id<MTL4PipelineDataSetSerializer> serializer = nil;
	~PipelineArchiveCache() { close(); }
	void close() {
		[serializer release]; serializer = nil;
		[_directory release]; _directory = nil;
		[_identity release]; _identity = nil;
	}
	void open(id<MTLDevice> device) {
		close();
		// Shader validation instruments compiled code. On macOS 27 its captured
		// binaries cannot be reopened as an archive; keep those runs uncached.
		const char *validation = getenv("MTL_SHADER_VALIDATION");
		if (validation && *validation && strcmp(validation, "0")) return;
		const char *overridePath = getenv("SCUMMVM_METAL4_CACHE_DIR");
		NSString *directory = overridePath ? [NSString stringWithUTF8String:overridePath] :
			[NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject
				stringByAppendingPathComponent:@"org.scummvm.metal4"];
		if (!directory.isAbsolutePath) return; // Empty override disables disk caching.
		NSFileManager *files = NSFileManager.defaultManager;
		if (![files createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil]) return;
		NSDictionary *attributes = [files attributesOfItemAtPath:directory error:nil];
		if (![attributes[NSFileType] isEqual:NSFileTypeDirectory]) return;
		MTL4PipelineDataSetSerializerDescriptor *desc = [[[MTL4PipelineDataSetSerializerDescriptor alloc] init] autorelease];
		desc.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
		serializer = [device newPipelineDataSetSerializerWithDescriptor:desc];
		if (!serializer) return;
		_directory = [directory copy];
		// Schema includes all compile options used by the renderer. OS build and
		// device identity isolate binaries across driver/hardware changes.
		_identity = [[NSString stringWithFormat:@"scummvm-metal4-v2-msl4-reflection;%@;%@;%llu",
			NSProcessInfo.processInfo.operatingSystemVersionString, device.name, device.registryID] copy];
	}
	static NSData *digest(NSData *data) {
		unsigned char hash[CC_SHA256_DIGEST_LENGTH];
		CC_SHA256(data.bytes, (CC_LONG)data.length, hash);
		return [NSData dataWithBytes:hash length:sizeof(hash)];
	}
	NSString *path(NSData *shaderIdentity, const void *state, NSUInteger size) const {
		if (!_directory) return nil;
		NSMutableData *data = [NSMutableData dataWithData:[_identity dataUsingEncoding:NSUTF8StringEncoding]];
		[data appendData:shaderIdentity]; [data appendBytes:state length:size];
		NSData *hash = digest(data);
		NSMutableString *name = [NSMutableString string];
		for (NSUInteger i = 0; i < hash.length; ++i) [name appendFormat:@"%02x", ((const byte *)hash.bytes)[i]];
		[name appendString:@".metal4cache"];
		return [_directory stringByAppendingPathComponent:name];
	}
	id<MTLRenderPipelineState> load(id<MTLDevice> device, NSString *path, MTL4RenderPipelineDescriptor *descriptor,
			Statistics &counters) {
		if (!path) return nil;
		NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
		if (!attributes) return nil;
		if (![attributes[NSFileType] isEqual:NSFileTypeRegular] || [attributes fileSize] > kByteLimit) {
			++counters.pipelineArchiveRejects; return nil;
		}
		// Metal's archive parser may abort on malformed input. Check our checksum
		// before giving it a file; missing metadata is a cache miss, never fatal.
		byte expected[CC_SHA256_DIGEST_LENGTH];
		NSData *contents = [NSData dataWithContentsOfFile:path];
		if (!contents || getxattr(path.fileSystemRepresentation, "org.scummvm.metal4.sha256", expected, sizeof(expected), 0, 0) != sizeof(expected) ||
			memcmp(expected, digest(contents).bytes, sizeof(expected))) {
			++counters.pipelineArchiveRejects; return nil;
		}
		NSError *failure = nil;
		id<MTL4Archive> archive = [device newArchiveWithURL:[NSURL fileURLWithPath:path] error:&failure];
		id<MTLRenderPipelineState> pipeline = archive ? [archive newRenderPipelineStateWithDescriptor:descriptor error:&failure] : nil;
		[archive release];
		if (pipeline) ++counters.pipelineArchiveHits; else ++counters.pipelineArchiveRejects;
		return pipeline;
	}
	bool store(id<MTLDevice> device, NSString *path, Statistics &counters) {
		if (!path || !serializer) return false;
		// Serialize and replace while holding a process-shared lock. Multiple
		// viewports/processes cannot race the capacity check or final rename.
		NSString *lockPath = [_directory stringByAppendingPathComponent:@"cache.lock"];
		int fd = ::open(lockPath.fileSystemRepresentation, O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
		if (fd < 0) return false;
		if (flock(fd, LOCK_EX) != 0) { ::close(fd); return false; }
		bool stored = false;
		NSFileManager *files = NSFileManager.defaultManager;
		NSString *temporary = [_directory stringByAppendingPathComponent:[@"pending-" stringByAppendingString:NSUUID.UUID.UUIDString]];
		NSError *failure = nil;
		if ([serializer serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:temporary] error:&failure]) {
			// Never publish an archive rejected by this device/OS, even if a
			// validation mode was enabled outside the environment above.
			id<MTL4Archive> verified = [device newArchiveWithURL:[NSURL fileURLWithPath:temporary] error:nil];
			if (!verified) {
				[files removeItemAtPath:temporary error:nil]; flock(fd, LOCK_UN); ::close(fd); return false;
			}
			[verified release];
			NSDictionary *incoming = [files attributesOfItemAtPath:temporary error:nil];
			uint64 bytes = [incoming fileSize];
			if (!bytes || bytes > kByteLimit) {
				[files removeItemAtPath:temporary error:nil]; flock(fd, LOCK_UN); ::close(fd); return false;
			}
			NSMutableArray *entries = [NSMutableArray array];
			uint64 total = bytes;
			for (NSString *name in [files contentsOfDirectoryAtPath:_directory error:nil]) {
				if (![name hasSuffix:@".metal4cache"] || name.length != 76) continue;
				NSString *entryPath = [_directory stringByAppendingPathComponent:name];
				NSDictionary *info = [files attributesOfItemAtPath:entryPath error:nil];
				if (![info[NSFileType] isEqual:NSFileTypeRegular]) continue;
				if ([entryPath isEqual:path]) continue;
				total += [info fileSize];
				[entries addObject:@{@"path":entryPath, @"size":info[NSFileSize], @"date":info[NSFileModificationDate]}];
			}
			[entries sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
				return [a[@"date"] compare:b[@"date"]];
			}];
			NSUInteger count = entries.count + 1;
			for (NSDictionary *entry in entries) {
				if (total <= kByteLimit && count <= kEntryLimit) break;
				if ([files removeItemAtPath:entry[@"path"] error:nil]) { total -= [entry[@"size"] unsignedLongLongValue]; --count; }
			}
			NSData *contents = [NSData dataWithContentsOfFile:temporary];
			NSData *checksum = contents ? digest(contents) : nil;
			bool sealed = checksum && setxattr(temporary.fileSystemRepresentation, "org.scummvm.metal4.sha256", checksum.bytes, checksum.length, 0, 0) == 0;
			if (sealed && bytes && total <= kByteLimit && count <= kEntryLimit &&
				::rename(temporary.fileSystemRepresentation, path.fileSystemRepresentation) == 0) {
				++counters.pipelineArchiveWrites; stored = true;
			}
		}
		[files removeItemAtPath:temporary error:nil];
		flock(fd, LOCK_UN); ::close(fd);
		return stored;
	}
private:
	enum { kByteLimit = 64 * 1024 * 1024, kEntryLimit = 256 };
	NSString *_directory = nil, *_identity = nil;
};

} } // namespace Graphics::Metal
#endif
