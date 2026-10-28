#import "GuestEntitlementsConversion.h"
#import <Foundation/Foundation.h>
#include "../ProcessSupport/GuestEntitlements.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <xpc/xpc.h>

// Native iOS 26.1 libxpc's exported wrapper validates [bytes, bytes + size) with CoreEntitlements, then creates an XPC dictionary.
// Its simulator counterpart is a NULL-returning stub; call this only in the host, which retains the native shared cache.
extern xpc_object_t xpc_create_from_ce_der(const void *bytes, size_t size) OS_OBJECT_RETURNS_RETAINED;

typedef struct { unsigned nodes; size_t bytes; } ConversionBudget;

static id propertyListValue(xpc_object_t object, unsigned depth, ConversionBudget *budget) {
    if (!object || depth > 64 || ++budget->nodes > 65536) return nil;
    xpc_type_t type = xpc_get_type(object);
    if (type == XPC_TYPE_BOOL) return @(xpc_bool_get_value(object));
    if (type == XPC_TYPE_INT64) return @(xpc_int64_get_value(object));
    if (type == XPC_TYPE_UINT64) return @(xpc_uint64_get_value(object));
    if (type == XPC_TYPE_STRING || type == XPC_TYPE_DATA) {
        size_t size = type == XPC_TYPE_STRING ? xpc_string_get_length(object) : xpc_data_get_length(object);
        if (size > LSB_GUEST_ENTITLEMENTS_MAX_XML_BLOB_SIZE - budget->bytes) return nil;
        budget->bytes += size;
        return type == XPC_TYPE_STRING ? [[NSString alloc] initWithBytes:xpc_string_get_string_ptr(object) length:size encoding:NSUTF8StringEncoding] :
            [NSData dataWithBytes:xpc_data_get_bytes_ptr(object) length:size];
    }
    if (type == XPC_TYPE_ARRAY) {
        size_t count = xpc_array_get_count(object);
        if (count > 65536 - budget->nodes) return nil;
        NSMutableArray *array = [NSMutableArray arrayWithCapacity:count];
        for (size_t i = 0; i < count; ++i) {
            id value = propertyListValue(xpc_array_get_value(object, i), depth + 1, budget);
            if (!value) return nil;
            [array addObject:value];
        }
        return [array copy];
    }
    if (type == XPC_TYPE_DICTIONARY) {
        if (xpc_dictionary_get_count(object) > 65536 - budget->nodes) return nil;
        NSMutableDictionary *dictionary = [NSMutableDictionary dictionary];
        bool complete = xpc_dictionary_apply(object, ^bool(const char *key, xpc_object_t child) {
            size_t length = strlen(key);
            if (length > LSB_GUEST_ENTITLEMENTS_MAX_XML_BLOB_SIZE - budget->bytes) return false;
            budget->bytes += length;
            NSString *name = [[NSString alloc] initWithBytes:key length:length encoding:NSUTF8StringEncoding];
            id value = propertyListValue(child, depth + 1, budget);
            if (!name || !value || dictionary[name]) return false;
            dictionary[name] = value;
            return true;
        });
        return complete ? [dictionary copy] : nil;
    }
    return nil;
}

int LSBCopyXMLFromDEREntitlementsWithDictionary(const void *der, size_t derSize, void **xml, size_t *xmlSize, NSDictionary **dictionary) {
    if (xml) *xml = NULL;
    if (xmlSize) *xmlSize = 0;
    if (dictionary) *dictionary = nil;
    if (!xml || !xmlSize || !LSBGuestEntitlementsHasValidBlobHeader(der, derSize)) return EINVAL;
    const unsigned char *bytes = der;
    int error = LSBValidateGuestEntitlementsDER(bytes + 8, derSize - 8);
    if (error) return error;
    NSDictionary *decoded = nil;
    @autoreleasepool {
        xpc_object_t object = xpc_create_from_ce_der(bytes + 8, derSize - 8);
        if (!object || xpc_get_type(object) != XPC_TYPE_DICTIONARY) return EINVAL;
        ConversionBudget budget = {0};
        decoded = propertyListValue(object, 0, &budget);
        if (!decoded) return EINVAL;
        NSData *data = [NSPropertyListSerialization dataWithPropertyList:decoded format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
        if (!data) return EINVAL;
        if (data.length > LSB_GUEST_ENTITLEMENTS_MAX_XML_BLOB_SIZE - 8) return E2BIG;
        size_t size = data.length + 8;
        unsigned char *result = malloc(size);
        if (!result) return ENOMEM;
        const unsigned char header[] = {0xfa, 0xde, 0x71, 0x71, size >> 24, size >> 16, size >> 8, size};
        memcpy(result, header, sizeof(header));
        memcpy(result + sizeof(header), data.bytes, data.length);
        *xml = result;
        *xmlSize = size;
    }
    if (dictionary) *dictionary = decoded; // Publish outside the inner pool so the autoreleasing output remains valid for its caller.
    return 0;
}

int LSBCopyXMLFromDEREntitlements(const void *der, size_t derSize, void **xml, size_t *xmlSize) {
    return LSBCopyXMLFromDEREntitlementsWithDictionary(der, derSize, xml, xmlSize, NULL);
}
