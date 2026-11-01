#pragma once

#import <Foundation/Foundation.h>
#import <Security/Security.h>

NS_ASSUME_NONNULL_BEGIN

// Returns an unpadded superblob for the exact existing allocation; never modifies inputFD's bytes, mode or seek offset.
// inputFD must be a read-only regular file. Only one private Security scratch copy is created and removed before returning.
// The caller must validate the input generation, write into a new staging inode, and register it before atomic publication.
NSData *_Nullable LSBCreateRuntimeSignature(int inputFD, uint64_t sliceOffset, uint64_t blobOffset, uint64_t blobSize,
                                           SecIdentityRef identity, NSArray *certificateChain, NSString *identifier, NSError **error);

// Only call on a service-owned staging copy, never the inode currently mapped or opened by a guest.
// Signs thin/fat arm64 Mach-O slices or a single-file arm64 iOS simulator cache. All bytes outside existing
// signature allocations are preserved. Insufficient capacity fails with ENOSPC; unsigned/malformed inputs are rejected.
// certificateChain is the kSecImportItemCertChain from the same P12 import, including any supplied intermediates.
BOOL LSBSignRuntimeBinary(NSURL *stagedFile, SecIdentityRef identity, NSArray *certificateChain, NSString *identifier, NSError **error);

// Exact failed F_ADDFILESIGS_RETURN allocation; blobOffset is relative to sliceOffset. Other fat slices remain byte-identical.
BOOL LSBSignRuntimeBinarySlice(NSURL *stagedFile, uint64_t sliceOffset, uint64_t blobOffset, uint64_t blobSize,
                              SecIdentityRef identity, NSArray *certificateChain, NSString *identifier, NSError **error);

NS_ASSUME_NONNULL_END
